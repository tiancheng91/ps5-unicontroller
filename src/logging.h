#ifndef UC_LOGGING_H
#define UC_LOGGING_H
#include "../vendor/rxi-log/log.h"

/* Preserve compile-time printf checks without editing the vendored API. */
void log_log(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Single event-loop logger. Configuration uses the existing settings values:
 * 0=Off, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Trace (rxi uses reversed values). */
void uc_log_init(void);
void uc_log_set_level(unsigned level);
int uc_log_open(const char *path);
void uc_log_close(void);
/* Read complete recent lines from the active runtime log, filtering with the
 * existing 0=Off..5=Trace levels. Returns bytes (including zero), or -1. */
int uc_log_tail(char *buffer, size_t capacity, unsigned level);

#define uc_log_at(level, ...)                                                                      \
    do {                                                                                           \
        uc_log_init();                                                                             \
        log_log(5 - (int)(level), __FILE__, __LINE__, __VA_ARGS__);                                \
    } while (0)
#define uc_log_error(...) uc_log_at(1, __VA_ARGS__)
#define uc_log_warn(...) uc_log_at(2, __VA_ARGS__)
#define uc_log_info(...) uc_log_at(3, __VA_ARGS__)
#define uc_log_debug(...) uc_log_at(4, __VA_ARGS__)
#define uc_log_trace(...) uc_log_at(5, __VA_ARGS__)
#endif
