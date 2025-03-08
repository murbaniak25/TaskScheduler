#define _POSIX_C_SOURCE 200112L

#include "logger.h"
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <semaphore.h>
#include <errno.h>

atomic_int toggle_flag = ATOMIC_VAR_INIT(0);
atomic_int logging_enabled = ATOMIC_VAR_INIT(1);
static pthread_t child_tid;
volatile sig_atomic_t exit_flag = 0;
static FILE* ptr_do_pliku = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t dump_sem;
static sem_t child_sem;
atomic_int curr_lvl = ATOMIC_VAR_INIT(1);
atomic_int zam_fl = ATOMIC_VAR_INIT(0);

void czas_na_str(char *buf, size_t buf_size) {
    time_t czas_teraz = time(NULL);
    struct tm rozbity_czas;
    localtime_r(&czas_teraz, &rozbity_czas);
    strftime(buf, buf_size, "%Y%m%d_%H%M%S", &rozbity_czas);
}

void logger_log(const char *format, ...) {
    pthread_mutex_lock(&log_mutex);
    if (ptr_do_pliku != NULL && atomic_load(&logging_enabled)) {
        char bufor_na_czas[64];
        czas_na_str(bufor_na_czas, sizeof(bufor_na_czas));
        fprintf(ptr_do_pliku, "[%s] [Akutalny poziom logowania: %d] ", bufor_na_czas, atomic_load(&curr_lvl));
        va_list args;
        va_start(args, format);
        vfprintf(ptr_do_pliku, format, args);
        va_end(args);
        fprintf(ptr_do_pliku, "\n");
        fflush(ptr_do_pliku);
    }
    pthread_mutex_unlock(&log_mutex);
}

void handler_exit(int signo) {
    (void)signo;
    exit_flag = 1;
}

void handler_dump(int signo, siginfo_t *info, void *context) {
    (void)signo; (void)info; (void)context;
    sem_post(&dump_sem);
}

void handler_toggle(int signo) {
    (void)signo;
    atomic_store(&toggle_flag, 1);
    sem_post(&child_sem);
}

void handler_change(int signo) {
    (void)signo;
    atomic_store(&zam_fl, 1);
    sem_post(&child_sem);
}

void* child_thread_func(void* arg) {
    (void)arg;
    sigset_t child_mask;
    sigfillset(&child_mask);
    sigdelset(&child_mask, SIGRTMIN + 1);
    sigdelset(&child_mask, SIGRTMIN + 2);
    pthread_sigmask(SIG_SETMASK, &child_mask, NULL);

    while (1) {
        //podmianka na semafor
        sem_wait(&child_sem);
        if (atomic_load(&toggle_flag) == 1) {
            int enabled = atomic_load(&logging_enabled);
            enabled = !enabled;
            atomic_store(&logging_enabled, enabled);
            printf("Watek poboczny: %d (Syngal: SIGRTMIN+1)\n", enabled);
            logger_log("Przestawiono logowanie na %d (Sygnal: SIGRTMIN+1)", enabled);
            atomic_store(&toggle_flag, 0);
        }
        if (atomic_load(&zam_fl) == 1) {
            int level = atomic_load(&curr_lvl);
            if (level < 2)
                level++;
            else
                level = 0;
            atomic_store(&curr_lvl, level);
            printf("Watek poboczny: Zmieniono poziom logowania na %d (Sygnal: SIGRTMIN+2)\n", level);
            logger_log("Poziom logowanaia zmieniony na: %d", level);
            atomic_store(&zam_fl, 0);
        }
    }
    return NULL;
}

void dumpy(void) {
    char timebuf[64];
    czas_na_str(timebuf, sizeof(timebuf));
    char filename[128];
    snprintf(filename, sizeof(filename), "dump_%s.txt", timebuf);
    FILE *f = fopen(filename, "w");
    if (f) {
        fprintf(f, "Przykładowy dump stanu aplikacji\n");
        fclose(f);
        printf("Glowny watek: Plik dump '%s' wygenerowano.\n", filename);
        logger_log("Plik dump '%s' wygenerowano.", filename);
    } else {
        perror("fopen dump file");
    }
}

int logger_init(void) {
    if (sem_init(&dump_sem, 0, 0) == -1) {
        perror("sem_init dump_sem");
        return -1;
    }
    if (sem_init(&child_sem, 0, 0) == -1) {
        perror("sem_init child_sem");
        return -1;
    }

    struct sigaction act;
    sigset_t set;
    sigfillset(&set);
    
    act.sa_sigaction = handler_dump;
    act.sa_flags = SA_SIGINFO;
    act.sa_mask = set;
    if (sigaction(SIGRTMIN, &act, NULL) == -1) {
        perror("sigaction SIGRTMIN");
        return -1;
    }
    
    act.sa_handler = handler_toggle;
    act.sa_flags = 0;
    act.sa_mask = set;
    if (sigaction(SIGRTMIN + 1, &act, NULL) == -1) {
        perror("sigaction SIGRTMIN+1");
        return -1;
    }
    
    act.sa_handler = handler_change;
    act.sa_flags = 0;
    act.sa_mask = set;
    if (sigaction(SIGRTMIN + 2, &act, NULL) == -1) {
        perror("sigaction SIGRTMIN+2");
        return -1;
    }
    
    struct sigaction exit_act;
    exit_act.sa_handler = handler_exit;
    sigemptyset(&exit_act.sa_mask);
    exit_act.sa_flags = 0;
    if (sigaction(SIGINT, &exit_act, NULL) == -1) {
        perror("sigaction SIGINT");
        return -1;
    }
    
    sigset_t main_mask;
    sigfillset(&main_mask);
    sigdelset(&main_mask, SIGRTMIN);
    sigdelset(&main_mask, SIGINT);
    pthread_sigmask(SIG_SETMASK, &main_mask, NULL);
    
    if (pthread_create(&child_tid, NULL, child_thread_func, NULL) != 0) {
        perror("pthread_create");
        return -1;
    }

    ptr_do_pliku = fopen("app.log", "a");
    if (!ptr_do_pliku) {
        perror("fopen app.log");
        return -1;
    }

    return 0;
}

void logger_run(void) {
    while (!exit_flag) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  /* Timeout 1 sekundy */
        int ret = sem_timedwait(&dump_sem, &ts);
        if (ret == 0) {
            printf("Glowny watek: Sygnal dump otrzymano (SIGRTMIN)\n");
            logger_log("Otrzymano sygnal dump(SIGRTMIN)");
            dumpy();
        } else {
            if (errno != ETIMEDOUT && errno != EINTR) {
                perror("sem_timedwait dump_sem");
            }
        }
    }
}

void logger_cleanup(void) {
    pthread_cancel(child_tid);
    pthread_join(child_tid, NULL);
    if (ptr_do_pliku) {
        fclose(ptr_do_pliku);
        ptr_do_pliku = NULL;
    }
    sem_destroy(&dump_sem);
    sem_destroy(&child_sem);
}
