/* Fault injection lives only in the host test binary, never the payload. */
#include "../../src/web/runtime.h"
#include <sys/socket.h>
#include <errno.h>
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
#define uc_backend_power_state fault_power
#define accept fault_accept
#define bind fault_bind
#include "../../src/web/runtime.c"
