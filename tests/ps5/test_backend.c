#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#define clock_gettime test_clock_gettime
#include "../../src/ps5/backend.c"
static uint64_t fake_ms = 10000;
int test_clock_gettime(clockid_t which, struct timespec *time)
{
    (void)which;
    uint64_t current = fake_ms++;
    time->tv_sec = current / 1000;
    time->tv_nsec = (current % 1000) * 1000000;
    return 0;
}
static uint64_t auth = 7;
static uint8_t caps[16];
static int removed, inserted, logged_in = 1, power_available;
static uint64_t power_bits = 1000;
static PadData frame;
int32_t sceUserServiceInitialize(void *p)
{
    (void)p;
    return 0;
}
int32_t sceUserServiceGetLoginUserIdList(int32_t users[4])
{
    users[0] = logged_in ? 42 : -1;
    users[1] = users[2] = users[3] = -1;
    return 0;
}
int32_t scePadInit(void)
{
    return 0;
}
int32_t scePadSetProcessPrivilege(int32_t p)
{
    (void)p;
    return 0;
}
int32_t scePadVirtualDeviceAddDevice(void *p, int32_t t)
{
    (void)p;
    (void)t;
    return 0;
}
int32_t scePadVirtualDeviceInsertData(int32_t h, const void *p)
{
    assert(h == 123);
    frame = *(const PadData *)p;
    inserted++;
    return 0;
}
int32_t scePadVirtualDeviceDeleteDevice(int32_t h)
{
    assert(h == 123);
    removed++;
    return 0;
}
uint64_t kernel_get_ucred_authid(pid_t p)
{
    (void)p;
    return auth;
}
int32_t kernel_get_ucred_caps(pid_t p, uint8_t out[16])
{
    (void)p;
    memcpy(out, caps, 16);
    return 0;
}
int32_t kernel_set_ucred_authid(pid_t p, uint64_t value)
{
    (void)p;
    auth = value;
    return 0;
}
int32_t kernel_set_ucred_caps(pid_t p, const uint8_t value[16])
{
    (void)p;
    memcpy(caps, value, 16);
    return 0;
}
int sceKernelOpenEventFlag(intptr_t *f, const char *n)
{
    (void)n;
    if (!power_available)
        return -1;
    *f = 1;
    return 0;
}
int sceKernelPollEventFlag(intptr_t f, uint64_t b, unsigned m, uint64_t *r)
{
    (void)f;
    (void)b;
    (void)m;
    if (!power_available)
        return -1;
    *r = power_bits;
    return 0;
}
int sceKernelCloseEventFlag(intptr_t f)
{
    (void)f;
    return 0;
}
int main(void)
{
    candidate(
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x7b][type:1][subType:22][capabilityBattery:1]");
    assert(matches == 0);
    candidate(
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x7b][type:1][subType:22][capabilityBattery:0]");
    assert(matches == 1 && device == 123);
    candidate(
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x7c][type:1][subType:22][capabilityBattery:0]");
    assert(matches == 2);
    ready = 1;
    phase = 4;
    device = 123;
    owner = 42;
    token = 9;
    int16_t axes[4] = {0, -32768, 32767, 0};
    uint16_t triggers[2] = {0, 65535};
    assert(uc_backend_submit(8, 1, axes, triggers) < 0);
    assert(inserted == 0);
    assert(uc_backend_submit(9, 1, axes, triggers) == 0);
    assert(uc_backend_submit(9, 0x10000, axes, triggers) == 0);
    assert(frame.buttons == 0x10000); /* PS key reaches ScePadData unchanged. */
    assert(frame.axes[0] == 128 && frame.axes[1] == 0 && frame.axes[2] == 255 &&
           frame.triggers[1] == 255);
    logged_in = 0;
    assert(uc_backend_submit(9, 1, axes, triggers) < 0);
    int16_t zero_axes[4] = {0};
    uint16_t zero_triggers[2] = {0};
    assert(uc_backend_submit(9, 0, zero_axes, zero_triggers) == 0);
    assert(uc_backend_delete(8) < 0 && removed == 0);
    assert(uc_backend_delete(9) == 0 && removed == 1 && frame.buttons == 0 && frame.axes[0] == 128);
    assert(uc_backend_submit(9, 1, axes, triggers) < 0);
    phase = 3;
    owner = 42;
    cancelled = 0;
    assert(uc_backend_cancel_pending() == 2 && cancelled == 1);
    phase = 0;
    suspend_event = 1;
    assert(uc_backend_suspend() == 1);
    assert(uc_backend_suspend() == 0);
    assert(uc_backend_power_state() == -1);
    /* Clock advances inside candidate(): poll must refresh time after reading,
     * never underflow an earlier timestamp into an expired ambiguity window. */
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    fd = pipe_fds[0];
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    const char event[] =
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x7b][type:1][subType:22][capabilityBattery:0]\n";
    assert(write(pipe_fds[1], event, sizeof(event) - 1) == (ssize_t)sizeof(event) - 1);
    phase = 3;
    matches = 0;
    cancelled = 1;
    device = 0;
    started = fake_ms;
    last_poll = fake_ms;
    uc_backend_poll();
    assert(phase == 3 && matches == 1 && !identified);
    fake_ms += 300;
    uc_backend_poll();
    assert(phase == 0 && removed == 2);
    close(pipe_fds[1]);
    assert(!ambiguity_elapsed(100, 101));

    /* Rest/unknown cannot create, stable wake still requires signed-in user. */
    phase = 0;
    ready = 1;
    logged_in = 1;
    uint64_t new_handle = 99;
    power_status = -1;
    assert(uc_backend_create(42, &new_handle) < 0 && new_handle == 0 && phase == 0);
    power_status = 1;
    assert(uc_backend_create(42, &new_handle) < 0 && phase == 0);
    power_status = 0;
    working_since = fake_ms;
    assert(uc_backend_create(42, &new_handle) == 2 && phase == 0);
    power_available = 1;
    power_bits = 500;
    uc_backend_poll();
    assert(working_since == 0 && power_status == 1);
    power_bits = 1000;
    uc_backend_poll();
    assert(working_since != 0 && phase == 0);
    working_since = fake_ms - 6000;
    logged_in = 0;
    assert(uc_backend_create(42, &new_handle) < 0 && phase == 0);
    assert(phase == 0); /* Wake/login never automatically creates an output. */
    return 0;
}
