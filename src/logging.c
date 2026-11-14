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
    fprintf(destination, "%-19s %-5s %s:%d %s\n", timestamp, log_level_string(event->level),
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

int uc_log_tail(char *buffer, size_t capacity, unsigned level)
{
    if (!buffer || capacity < 2 || level > 5)
        return -1;
    buffer[0] = 0;
    if (!level)
        return 0;
    if (!output_path[0])
        return -1;
    FILE *file = fopen(output_path, "rb");
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return -1;
    }
    size_t limit = capacity - 1 < 32768 ? capacity - 1 : 32768;
    long start = (unsigned long)size > limit ? size - (long)limit : 0;
    if (fseek(file, start ? start - 1 : 0, SEEK_SET)) {
        fclose(file);
        return -1;
    }
    /* Never expose a partial first record or cut through UTF-8 text. */
    if (start && fgetc(file) != '\n') {
        int byte;
        do {
            byte = fgetc(file);
        } while (byte != '\n' && byte != EOF);
    }
    size_t length = fread(buffer, 1, limit, file);
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        buffer[0] = 0;
        return -1;
    }
    buffer[length] = 0;
    static const char *labels[] = {"", "ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};
    size_t input = 0, used = 0;
    while (input < length) {
        const char *newline = memchr(buffer + input, '\n', length - input);
        if (!newline)
            break;
        size_t line = (size_t)(newline - (buffer + input)) + 1;
        unsigned severity = 1; /* Preserve unstructured stderr diagnostics. */
        if (line >= 26) {
            for (unsigned i = 1; i <= 5; ++i)
                if (!memcmp(buffer + input + 20, labels[i], 5)) {
                    severity = i;
                    break;
                }
        }
        if (severity <= level) {
            memmove(buffer + used, buffer + input, line);
            used += line;
        }
        input += line;
    }
    buffer[used] = 0;
    return (int)used;
}
