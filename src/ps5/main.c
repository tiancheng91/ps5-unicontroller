#include "../logging.h"
#include "uc_backend.h"
#include "../drivers/bt.h"
#include "../drivers/usb.h"
#include "../web/runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int instance_fd = -1;

static void shutdown_payload(void)
{
    (void)uc_bt_stop();
    (void)uc_usb_stop();
    uc_backend_shutdown();
    uc_log_info("UniController: stopped; owned output cleanup attempted");
    uc_log_close();
    if (instance_fd >= 0) {
        close(instance_fd);
        instance_fd = -1;
    }
}

int main(void)
{
    (void)umask(077); /* Pairing keys and temporary files remain private. */
    if (mkdir("/data/unicontroller", 0700) != 0 && errno != EEXIST) {
        uc_log_warn("UniController: data directory unavailable errno=%d", errno);
        return 1;
    }
    instance_fd = open("/data/unicontroller/instance.lock", O_RDWR | O_CREAT, 0600);
    if (instance_fd < 0 || flock(instance_fd, LOCK_EX | LOCK_NB) != 0) {
        uc_log_warn("UniController: already running or instance lock unavailable errno=%d", errno);
        if (instance_fd >= 0)
            close(instance_fd);
        return 1;
    }
    struct stat log_stat;
    if (stat("/data/unicontroller/runtime.log", &log_stat) == 0 && log_stat.st_size >= 1024 * 1024)
        (void)rename("/data/unicontroller/runtime.log", "/data/unicontroller/runtime.log.1");
    int log_fd = open("/data/unicontroller/runtime.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
        if (log_fd >= 0)
            close(log_fd);
        close(instance_fd);
        return 1;
    }
    if (log_fd != STDERR_FILENO)
        close(log_fd);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!uc_log_open("/data/unicontroller/runtime.log")) {
        uc_log_error("runtime log open failed errno=%d", errno);
        close(instance_fd);
        return 1;
    }
    uc_log_info(
        "UniController 0.1.0: starting time=%lld pid=%d; physical inputs require manual start",
        (long long)time(NULL), (int)getpid());
    if (atexit(shutdown_payload) != 0) {
        close(instance_fd);
        return 1;
    }
    if (uc_backend_init() != 0) {
        uc_log_error("UniController: virtual output initialization failed");
        return 1;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    int result = uc_runtime_run("0.0.0.0", 7456, "/data/unicontroller/settings.json");
    if (result != 0)
        uc_log_error("UniController: HTTP startup/run failed errno=%d", errno);
    return result == 0 ? 0 : 1;
}
