#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <mqueue.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>
#include "logger.h"

#define MQ_NAME "/scheduler_mq"
#define MAX_MSG_SIZE 512
#define MAX_MSG_COUNT 10

enum {
    RELATYWNY = 0,
    ABS,
    CYKLICZNY
};

enum {
    DODAJ = 1,
    LISTA,
    ANULUJ,
    STOP
};

typedef struct {
    int cmd;
    int task_id;
    int schedule_type;
    long time_val;
    char command[256];
    char args[256];
    char kolejka_klienta[64];
} message_t;

typedef struct task {
    int id;
    int schedule_type;
    long time_val;
    char command[256];
    char args[256];
    timer_t timer;
    struct task* next;
} task_t;

static task_t* task_list = NULL;
static int next_task_id = 1;
pthread_mutex_t task_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void remove_task_from_list(int task_id) {
    task_t **indirect = &task_list;
    while (*indirect && (*indirect)->id != task_id) {
        indirect = &((*indirect)->next);
    }
    if (*indirect) {
        task_t *to_remove = *indirect;
        *indirect = to_remove->next;
        timer_delete(to_remove->timer);
        free(to_remove);
    }
}

void timer_callback(union sigval sv) {
    task_t* t = (task_t*)sv.sival_ptr;
    logger_log("Wykonywanie taska %d: %s %s", t->id, t->command, t->args);
    pid_t pid = fork();
    if (pid == 0) {

        execlp(t->command, t->command, t->args, (char*)NULL);
        exit(EXIT_FAILURE);
    }

    if (t->schedule_type != CYKLICZNY) {
        pthread_mutex_lock(&task_list_mutex);
        remove_task_from_list(t->id);
        pthread_mutex_unlock(&task_list_mutex);
    }
}
char* task_list_na_stringa() {
    pthread_mutex_lock(&task_list_mutex);
    task_t* cur = task_list;
    char* buffer = malloc(4096);
    if (!buffer) {
        pthread_mutex_unlock(&task_list_mutex);
        return NULL;
    }
    buffer[0] = '\0';
    while (cur) {
        char task_info[1024];
        snprintf(task_info, sizeof(task_info), "Task %d: typ=%d, time_val=%ld, command=%s, args=%s\n",
                 cur->id, cur->schedule_type, cur->time_val, cur->command, cur->args);
        strcat(buffer, task_info);
        cur = cur->next;
    }

    pthread_mutex_unlock(&task_list_mutex);
    return buffer;
}
void list_tasks(void) {
    pthread_mutex_lock(&task_list_mutex);
    task_t* cur = task_list;
    printf("Zaplanowane taski:\n");
    logger_log("Lista zadan:");
    while (cur) {
        printf("Task %d: typ=%d, time_val=%ld, command=%s, args=%s\n",
               cur->id, cur->schedule_type, cur->time_val, cur->command, cur->args);
        logger_log("Task %d: typ=%d, time_val=%ld, command=%s, args=%s",
                   cur->id, cur->schedule_type, cur->time_val, cur->command, cur->args);
        cur = cur->next;
    }
    pthread_mutex_unlock(&task_list_mutex);
}
int add_task(int schedule_type, long time_val, const char* command, const char* args) {
    task_t* nowy_task = malloc(sizeof(task_t));
    if (!nowy_task) return -1;
    nowy_task->id = next_task_id++;
    nowy_task->schedule_type = schedule_type;
    nowy_task->time_val = time_val;
    strncpy(nowy_task->command, command, sizeof(nowy_task->command)-1);
    nowy_task->command[sizeof(nowy_task->command)-1] = '\0';
    strncpy(nowy_task->args, args, sizeof(nowy_task->args)-1);
    nowy_task->args[sizeof(nowy_task->args)-1] = '\0';
    nowy_task->next = NULL;

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_callback;
    sev.sigev_value.sival_ptr = nowy_task;
    if (timer_create(CLOCK_REALTIME, &sev, &nowy_task->timer) == -1) {
        free(nowy_task);
        return -1;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    time_t now = time(NULL);
    long delay = 0;
    if (schedule_type == RELATYWNY) {
        delay = time_val;
    } else if (schedule_type == ABS) {
        delay = (time_val > now) ? time_val - now : 0;
    } else if (schedule_type == CYKLICZNY) {
        delay = time_val;
    }
    its.it_value.tv_sec = delay;
    if (schedule_type == CYKLICZNY) {
        its.it_interval.tv_sec = time_val;
    }
    if (timer_settime(nowy_task->timer, 0, &its, NULL) == -1) {
        timer_delete(nowy_task->timer);
        free(nowy_task);
        return -1;
    }
    pthread_mutex_lock(&task_list_mutex);
    nowy_task->next = task_list;
    task_list = nowy_task;
    pthread_mutex_unlock(&task_list_mutex);
    logger_log("Dodano zadanie %d: type=%d, time_val=%ld, command=%s, args=%s",
               nowy_task->id, schedule_type, time_val, command, args);
    return nowy_task->id;
}

int cancel_task(int task_id) {
    pthread_mutex_lock(&task_list_mutex);
    task_t* cur = task_list;
    task_t* prev = NULL;
    while (cur) {
        if (cur->id == task_id) {
            if (prev)
                prev->next = cur->next;
            else
                task_list = cur->next;
            timer_delete(cur->timer);
            free(cur);
            pthread_mutex_unlock(&task_list_mutex);
            logger_log("Anulowano zadanie o id: %d", task_id);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&task_list_mutex);
    return -1;
}

void cancel_all_tasks(void) {
    pthread_mutex_lock(&task_list_mutex);
    task_t* cur = task_list;
    while (cur) {
        timer_delete(cur->timer);
        task_t* temp = cur;
        cur = cur->next;
        free(temp);
    }
    task_list = NULL;
    pthread_mutex_unlock(&task_list_mutex);
    logger_log("Anulowano wszystkie taski");
}
void run_client(int argc, char *argv[]) {
    char kolejka_klienta[64];
    snprintf(kolejka_klienta, sizeof(kolejka_klienta), "/client_mq_%d", getpid());
    struct mq_attr attr = {0, MAX_MSG_COUNT, MAX_MSG_SIZE, 0};
    
    mqd_t client_mq = mq_open(kolejka_klienta, O_RDONLY | O_CREAT | O_EXCL, 0644, &attr);
    if (client_mq == (mqd_t)-1) {
        perror("mq_open client");
        exit(EXIT_FAILURE);
    }
    
    mqd_t server_mq = mq_open(MQ_NAME, O_WRONLY);
    if (server_mq == (mqd_t)-1) {
        perror("mq_open server");
        mq_close(client_mq);
        mq_unlink(kolejka_klienta);
        exit(EXIT_FAILURE);
    }
    
    message_t msg = {0};
    strncpy(msg.kolejka_klienta, kolejka_klienta, sizeof(msg.kolejka_klienta) - 1);
    
    if (strcmp(argv[1], "-a") == 0) {
        msg.cmd = DODAJ;
        if (strcmp(argv[2], "rel") == 0)
            msg.schedule_type = RELATYWNY;
        else if (strcmp(argv[2], "abs") == 0)
            msg.schedule_type = ABS;
        else if (strcmp(argv[2], "cyc") == 0)
            msg.schedule_type = CYKLICZNY;
        else {
            fprintf(stderr, "Niepoprawny typ zadania\n");
            mq_close(client_mq);
            mq_unlink(kolejka_klienta);
            exit(EXIT_FAILURE);
        }
        msg.time_val = atol(argv[3]);
        strncpy(msg.command, argv[4], sizeof(msg.command) - 1);
        
        char argbuf[256] = "";
        for (int i = 5; i < argc; i++) {
            strncat(argbuf, argv[i], sizeof(argbuf) - strlen(argbuf) - 1);
            if (i < argc - 1)
                strncat(argbuf, " ", sizeof(argbuf) - strlen(argbuf) - 1);
        }
        strncpy(msg.args, argbuf, sizeof(msg.args) - 1);
    } else if (strcmp(argv[1], "-l") == 0) {
        msg.cmd = LISTA;
    } else if (strcmp(argv[1], "-c") == 0) {
        msg.cmd = ANULUJ;
        msg.task_id = atoi(argv[2]);
    } else if (strcmp(argv[1], "-s") == 0) {
        msg.cmd = STOP;
    } else {
        fprintf(stderr, "Nieznana opcja\n");
        mq_close(client_mq);
        mq_unlink(kolejka_klienta);
        exit(EXIT_FAILURE);
    }
    if (mq_send(server_mq, (char*)&msg, sizeof(msg), 0) == -1) {
        perror("mq_send");
        mq_close(client_mq);
        mq_unlink(kolejka_klienta);
        exit(EXIT_FAILURE);
    }

    if (msg.cmd == LISTA) {
        char buffer[MAX_MSG_SIZE];
        ssize_t bytes_read = mq_receive(client_mq, buffer, sizeof(buffer), NULL);
        if (bytes_read >= 0) {
            buffer[bytes_read] = '\0';
            printf("Polecenie odebrano z serwera.\n");
            printf("Lista zadań:\n%s", buffer);
        } else {
            perror("mq_receive");
        }
    } else {
        printf("Polecenie wysłane na serwer.\n");
    }
    
    mq_close(client_mq);
    mq_unlink(kolejka_klienta);
}
void run_server(void) {
    mqd_t mq;
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MSG_COUNT;
    attr.mq_msgsize = sizeof(message_t);
    attr.mq_curmsgs = 0;

    mq = mq_open(MQ_NAME, O_RDONLY | O_CREAT | O_EXCL, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open server");
        exit(EXIT_FAILURE);
    }
    logger_log("Serwer uruchomiony");
    printf("Serwer uruchominy. Czekanie na polecenia...\n");

    int flagga = 1;
    while (flagga) {
        message_t msg;
        ssize_t bytes_read = mq_receive(mq, (char*)&msg, sizeof(msg), NULL);
        if (bytes_read >= 0) {
            switch (msg.cmd) {
                case DODAJ: {
                    int id = add_task(msg.schedule_type, msg.time_val, msg.command, msg.args);
                    if (id > 0)
                        printf("Dodano taska %d\n", id);
                    else
                        printf("Blad dodawania taska\n");
                    break;
                }
                case LISTA: {
                    char* task_list_str = task_list_na_stringa();
                    if (task_list_str) {
                        mqd_t client_mq = mq_open(msg.kolejka_klienta, O_WRONLY);
                        if (client_mq != (mqd_t)-1) {
                            mq_send(client_mq, task_list_str, strlen(task_list_str) + 1, 0);
                            mq_close(client_mq);
                        }
                        free(task_list_str);
                    }
                    break;
                }
                case ANULUJ: {
                    if (cancel_task(msg.task_id) == 0)
                        printf("Anulowanie taska %d\n", msg.task_id);
                    else
                        printf("Nie znaleziono taska o id: %d\n", msg.task_id);
                    break;
                }
                case STOP: {
                    printf("Stopping server...\n");
                    logger_log("Server stopping");
                    flagga = 0;
                    break;
                }
                default:
                    printf("Nieznane polecenie\n");
                    break;
            }
        } else {
            if (errno == EINTR)
                continue;
            perror("mq_receive");
        }
    }
    cancel_all_tasks();
    mq_close(mq);
    mq_unlink(MQ_NAME);
    logger_log("Server terminated");
    logger_cleanup();
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        if (logger_init() != 0) {
            fprintf(stderr, "Logger initialization failed\n");
            exit(EXIT_FAILURE);
        }
        run_server();
    } else {
        run_client(argc, argv);
    }
    return 0;
}

