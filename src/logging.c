#include "logging.h"
#include <string.h>

#define LOG_MAX (1L << 20)
#define MESSAGE_MAX 2048
static unsigned recording = 3;
static int initialized;
static FILE *output;
static char output_path[256];

/* Keep the old handle until a replacement exists. A failed rotation must not
 * silently discard the line or leave the logger pointing at a closed FILE. */
static void rotate(void)
{
    if (!output || ftell(output) < LOG_MAX)
        return;
    char previous[sizeof output_path + 2];
    snprintf(previous, sizeof previous, "%s.1", output_path);
    if (rename(output_path, previous) != 0)
        return;
    FILE *next = fopen(output_path, "a");
    if (!next) {
        (void)rename(previous, output_path);
        return;
    }
    fclose(output);
    output = next;
}
static void write_event(log_Event *event)
{
    if (!recording || event->level < 5 - (int)recording)
        return;
    char timestamp[32], message[MESSAGE_MAX];
    if (!event->time || !strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", event->time))
        snprintf(timestamp, sizeof timestamp, "time-unavailable");
    int length = vsnprintf(message, sizeof message, event->fmt, event->ap);
    if (length < 0)
        snprintf(message, sizeof message, "log_format_failed");
    else if ((size_t)length >= sizeof message) {
        static const char suffix[] = " [truncated]";
        memcpy(message + sizeof message - sizeof suffix, suffix, sizeof suffix);
    }
    /* Device metadata must not forge extra log records. */
    for (char *p = message; *p; ++p)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    rotate();
    FILE *destination = output ? output : stderr;
    fprintf(destination, "%s %-5s %s:%d %s\n", timestamp, log_level_string(event->level),
            event->file, event->line, message);
    fflush(destination);
}
void uc_log_init(void)
{
    if (initialized)
        return;
    initialized = 1;
    log_set_level(LOG_INFO);
    /* One callback for the process lifetime; open/close never consume slots. */
    if (log_add_callback(write_event, NULL, LOG_TRACE) == 0)
        log_set_quiet(true);
}
void uc_log_set_level(unsigned level)
{
    uc_log_init();
    if (level <= 5) {
        recording = level;
        log_set_level(level ? 5 - (int)level : LOG_FATAL + 1);
    }
}
int uc_log_open(const char *path)
{
    uc_log_init();
    if (!path)
        return 1;
    if (!*path || strlen(path) >= sizeof output_path)
        return 0;
    FILE *next = fopen(path, "a");
    if (!next)
        return 0;
    if (output)
        fclose(output);
    output = next;
    snprintf(output_path, sizeof output_path, "%s", path);
    return 1;
}
void uc_log_close(void)
{
    if (output)
        fclose(output);
    output = NULL;
    output_path[0] = 0;
}
