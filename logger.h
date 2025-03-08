#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

int logger_init(void);
void logger_run(void);
void logger_cleanup(void);
void logger_log(const char *format, ...);

#endif /* LOGGER_H */
