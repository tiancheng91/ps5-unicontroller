/* Fault injection lives only in the host test binary, never the payload. */
#include "../../src/web/runtime.h"
#include <sys/socket.h>
#include "../../src/drivers/bt.h"
#include "../../src/drivers/usb.h"
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

static int fault_accept(int fd, struct sockaddr *address, socklen_t *size)
{
    const char *path = getenv("UC_TEST_BREAK_LISTENER");
    if (path && !access(path, F_OK)) {
        unlink(path);
        close(fd);
        errno = EBADF;
        return -1;
    }
    return accept(fd, address, size);
}
static int fault_bind(int fd, const struct sockaddr *address, socklen_t size)
{
    const char *path = getenv("UC_TEST_FAIL_BIND");
    if (path && !access(path, F_OK)) {
        errno = EADDRINUSE;
        return -1;
    }
    return bind(fd, address, size);
}
static int fault_power(void)
{
    const char *path = getenv("UC_TEST_REST");
    return path && !access(path, F_OK) ? 1 : uc_backend_power_state();
}
/* A real USB manager must keep scanning throughout several simulated HCI waits. */
static unsigned scans;
static int empty_scan(void *ctx, uc_usb_device *device)
{
    (void)ctx;
    (void)device;
    ++scans;
    return 0;
}
static int unused_read(void *ctx, uint8_t *data, size_t size)
{
    (void)ctx;
    (void)data;
    (void)size;
    return -1;
}
static int unused_write(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    (void)data;
    (void)size;
    return -1;
}
static int absent(void *ctx)
{
    (void)ctx;
    return 0;
}
static void close_empty(void *ctx)
{
    (void)ctx;
}
static host_t *slow_open(const char *path, const host_events *events, void *context)
{
    (void)path;
    (void)context;
    unsigned before = scans;
    for (int i = 0; i < 8; i++) {
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
        events->idle(events->ud);
    }
    assert(scans >= before + 3 && uc_usb_running());
    return NULL; /* Failed HCI setup must leave the independent USB service running. */
}
/* Deliver one synthetic event after the platform gate has elapsed. */
static int fault_auto_due(unsigned delay_ms, int idle)
{
    const char *path = getenv("UC_TEST_AUTO_DUE");
    if (!idle || !path || delay_ms != 7000 || unlink(path))
        return 0;
    static const uc_usb_ops ops = {empty_scan, unused_read, unused_write, absent, close_empty};
    uc_usb_transport transport = {.ops = &ops};
    uc_bt_factory factory = {.open = slow_open};
    assert(!uc_usb_set_transport(&transport) && !uc_bt_set_factory(&factory, NULL));
    return 1;
}
#define uc_backend_auto_due fault_auto_due
#define uc_backend_power_state fault_power
#define accept fault_accept
#define bind fault_bind
#include "../../src/web/runtime.c"
