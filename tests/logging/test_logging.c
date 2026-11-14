#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "../../src/logging.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t read_all(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "r");
    assert(file);
    size_t length = fread(buffer, 1, capacity - 1, file);
    assert(!ferror(file));
    buffer[length] = 0;
    fclose(file);
    return length;
}
int main(void)
{
    char directory[] = "/tmp/uc-logging-XXXXXX";
    assert(mkdtemp(directory));
    char path[256], previous[258], buffer[8192];
    snprintf(path, sizeof path, "%s/runtime.log", directory);
    snprintf(previous, sizeof previous, "%s.1", path);
    assert(uc_log_open(path));
    uc_log_set_level(2);
    uc_log_info("hidden-info");
    uc_log_warn("visible-warn");
    uc_log_error("visible-error");
    read_all(path, buffer, sizeof buffer);
    assert(!strstr(buffer, "hidden-info"));
    assert(strstr(buffer, "WARN ") && strstr(buffer, "ERROR "));
    assert(strstr(buffer, "test_logging.c:") && strstr(buffer, "visible-warn"));
    assert(buffer[4] == '-' && buffer[7] == '-' && buffer[10] == ' ');
    size_t before = strlen(buffer);
    uc_log_set_level(0);
    uc_log_error("hidden-off");
    assert(read_all(path, buffer, sizeof buffer) == before);
    uc_log_set_level(5);
    uc_log_trace("visible-trace");
    uc_log_debug("visible-debug");
    uc_log_info("injected\nrecord\rcontrol\ttext");
    read_all(path, buffer, sizeof buffer);
    assert(strstr(buffer, "TRACE") && strstr(buffer, "DEBUG"));
    assert(strstr(buffer, "injected record control text"));
    assert(uc_log_tail(buffer, sizeof buffer, 2) > 0);
    assert(strstr(buffer, "visible-error") && strstr(buffer, "visible-warn"));
    assert(!strstr(buffer, "visible-trace") && !strstr(buffer, "visible-debug"));
    assert(uc_log_tail(buffer, sizeof buffer, 5) > 0 && strstr(buffer, "visible-trace"));
    assert(uc_log_tail(buffer, sizeof buffer, 0) == 0 && !buffer[0]);
    assert(uc_log_tail(buffer, sizeof buffer, 6) < 0);
    char tiny[128];
    int tail = uc_log_tail(tiny, sizeof tiny, 5);
    assert(tail > 0 && tail < (int)sizeof tiny && tiny[tail - 1] == '\n');
    assert(uc_log_tail(buffer, sizeof buffer, 5) > 0 && strstr(buffer, tiny));
    char large[5000];
    memset(large, 'x', sizeof large - 1);
    large[sizeof large - 1] = 0;
    uc_log_info("%s", large);
    read_all(path, buffer, sizeof buffer);
    assert(strstr(buffer, " [truncated]\n"));
    assert(!uc_log_open("/not/a/real/uc-directory/log"));
    uc_log_info("previous-output-still-open");
    read_all(path, buffer, sizeof buffer);
    assert(strstr(buffer, "previous-output-still-open"));
    uc_log_close();
    /* Reconfiguration must not exhaust rxi's callback slots or duplicate lines. */
    for (int i = 0; i < 40; i++) {
        assert(uc_log_open(path));
        uc_log_info("reopen=%d", i);
        uc_log_close();
    }
    read_all(path, buffer, sizeof buffer);
    const char *last = strstr(buffer, "reopen=39\n");
    assert(last && !strstr(last + 1, "reopen=39\n"));
    FILE *file = fopen(path, "a");
    assert(file && ftruncate(fileno(file), 1L << 20) == 0);
    fclose(file);
    assert(uc_log_open(path));
    uc_log_set_level(3);
    uc_log_info("after-rotation");
    uc_log_close();
    assert(access(previous, F_OK) == 0);
    assert(uc_log_tail(buffer, sizeof buffer, 3) < 0); /* Closed logger has no active file. */
    assert(uc_log_open(path));
    assert(uc_log_tail(buffer, sizeof buffer, 3) > 0 && strstr(buffer, "after-rotation"));
    uc_log_close();
    read_all(path, buffer, sizeof buffer);
    assert(strstr(buffer, "after-rotation") && !strstr(buffer, "visible-warn"));
    /* Closing a file restores stderr, rather than a closed stream. */
    int saved = dup(STDERR_FILENO);
    FILE *capture = tmpfile();
    assert(saved >= 0 && capture && dup2(fileno(capture), STDERR_FILENO) >= 0);
    uc_log_error("stderr-fallback");
    rewind(capture);
    assert(fgets(buffer, sizeof buffer, capture) && strstr(buffer, "stderr-fallback"));
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    fclose(capture);
    unlink(path);
    unlink(previous);
    rmdir(directory);
    puts("rxi logging: thresholds, format, truncation, reopen, rotation and fallback PASS");
    return 0;
}
