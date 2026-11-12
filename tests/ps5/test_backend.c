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
static int removed[256], inserted[256], feedback_calls[256];
static int logged_in = 1, power_available = 1, delete_failure = -1, feedback_failure = -1;
static uint64_t power_bits = 1000;
static PadData frames[256];
int32_t sceUserServiceInitialize(void *p)
{
    (void)p;
    return 0;
}
int32_t sceUserServiceGetLoginUserIdList(int32_t users[4])
{
    for (int i = 0; i < 4; i++)
        users[i] = logged_in ? 42 + i : -1;
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
    assert(h >= 120 && h < 256);
    frames[h] = *(const PadData *)p;
    inserted[h]++;
    return 0;
}
int32_t scePadVirtualDeviceDeleteDevice(int32_t h)
{
    assert(h >= 120 && h < 256);
    if (h == delete_failure)
        return -123;
    removed[h]++;
    return 0;
}
static int32_t feedback(int32_t handle, void *buffer)
{
    assert(handle >= 120 && handle < 256);
    assert((uintptr_t)buffer % _Alignof(uint64_t) == 0);
    feedback_calls[handle]++;
    if (handle == feedback_failure)
        return -44;
    uint8_t *bytes = buffer;
    bytes[3] = (uint8_t)handle;
    bytes[4] = (uint8_t)(handle + 1);
    return 0;
}
static int bind_mock(uint64_t device_id, int32_t user)
{
    assert(device_id >= 120 && device_id < 256);
    assert(user >= 42 && user <= 45);
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
int sceKernelOpenEventFlag(intptr_t *flag, const char *name)
{
    (void)name;
    if (!power_available)
        return -1;
    *flag = 1;
    return 0;
}
int sceKernelPollEventFlag(intptr_t flag, uint64_t bits, unsigned mode, uint64_t *result)
{
    (void)flag;
    (void)bits;
    (void)mode;
    if (!power_available)
        return -1;
    *result = power_bits;
    return 0;
}
int sceKernelCloseEventFlag(intptr_t flag)
{
    (void)flag;
    return 0;
}
static void prepare(void)
{
    ready = 1;
    power_status = 0;
    working_since = fake_ms - 6000;
    last_poll = fake_ms;
    bind_user = bind_mock;
    remote_setting = feedback;
}
/* Start at AddDevice's asynchronous wait to avoid real network I/O. All
 * identity matching, ambiguity timing, binding and publication run in poll. */
static int start_identification(int32_t user, unsigned raw_device)
{
    assert(!phase);
    int slot = -1;
    for (unsigned i = 0; i < 4; i++)
        if (!outputs[i].used) {
            slot = (int)i;
            break;
        }
    assert(slot >= 0);
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    fd = pipe_fds[0];
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    phase = 3;
    pending_slot = slot;
    owner = user;
    matches = 0;
    identified = 0;
    cancelled = 0;
    device = 0;
    started = fake_ms;
    last_poll = fake_ms;
    char event[192];
    int length = snprintf(
        event, sizeof(event),
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x%x][type:1][subType:22][capabilityBattery:0]\n",
        raw_device);
    assert(write(pipe_fds[1], event, (size_t)length) == length);
    return pipe_fds[1];
}
static uint64_t finish_identification(int writer, int32_t user)
{
    uc_backend_poll();
    assert(phase == 3 && matches == 1 && !identified);
    fake_ms += 300;
    uc_backend_poll();
    assert(phase == 4);
    close(writer);
    uint64_t handle = 0;
    assert(uc_backend_create(user, &handle) == 0 && handle);
    return handle;
}
static void isolation_and_rumble(void)
{
    prepare();
    uint64_t handles[4];
    for (unsigned i = 0; i < 4; i++) {
        int writer = start_identification(42 + (int)i, 123 + i);
        if (i) {
            int16_t axes[4] = {0};
            uint16_t triggers[2] = {0};
            assert(!uc_backend_submit(handles[0], 1, axes, triggers));
            uint64_t again = 0;
            assert(!uc_backend_create(42, &again) && again == handles[0]);
            assert(uc_backend_create(45, &again) == (i == 3 ? 2 : -2));
            assert(phase == 3); /* Another user's output did not replace the pending request. */
        }
        handles[i] = finish_identification(writer, 42 + (int)i);
        for (unsigned j = 0; j < i; j++)
            assert(handles[i] != handles[j]);
    }
    assert(!ambiguity_elapsed(100, 101));
    int16_t axes[4] = {0, -32768, 32767, 0};
    uint16_t triggers[2] = {0, 65535};
    assert(uc_backend_submit(9999, 1, axes, triggers) < 0);
    assert(!uc_backend_submit(handles[0], 0x10000, axes, triggers));
    assert(frames[123].buttons == 0x10000 && frames[123].axes[0] == 128 &&
           frames[123].axes[1] == 0 && frames[123].axes[2] == 255 &&
           frames[123].triggers[1] == 255);
    assert(!uc_backend_delete(0));
    assert(!removed[123] && !removed[124]);
    for (unsigned i = 0; i < 4; i++) {
        uint8_t strong = 0, weak = 0;
        assert(uc_backend_read_rumble(handles[i], 100, &strong, &weak) == 1);
        assert(strong == 123 + i && weak == 124 + i && feedback_calls[123 + i] == 1);
        assert(!uc_backend_read_rumble(handles[i], 115, &strong, &weak) && !strong && !weak);
    }
    uint8_t strong = 99, weak = 99;
    assert(uc_backend_read_rumble(9999, 116, &strong, &weak) < 0 && !strong && !weak);
    feedback_failure = 124;
    assert(uc_backend_read_rumble(handles[1], 116, &strong, &weak) < 0);
    assert(!uc_backend_read_rumble(handles[1], 132, &strong, &weak) && feedback_calls[124] == 2);
    assert(uc_backend_read_rumble(handles[0], 116, &strong, &weak) == 1 && strong == 123);
    assert(uc_backend_read_rumble(handles[0], 115, &strong, &weak) < 0);
    logged_in = 0;
    assert(uc_backend_submit(handles[0], 1, axes, triggers) < 0);
    assert(!uc_backend_read_rumble(handles[0], 132, &strong, &weak));
    int16_t zero_axes[4] = {0};
    uint16_t zero_triggers[2] = {0};
    assert(!uc_backend_submit(handles[0], 0, zero_axes, zero_triggers));
    assert(!uc_backend_delete(handles[0]) && removed[123] == 1);
    assert(uc_backend_read_rumble(handles[0], 148, &strong, &weak) < 0);
    assert(uc_backend_submit(handles[0], 1, axes, triggers) < 0);
    logged_in = 1;
    delete_failure = 124;
    assert(uc_backend_delete(handles[1]) < 0 && !removed[124]);
    assert(uc_backend_submit(handles[1], 1, axes, triggers) < 0);
    assert(!uc_backend_submit(handles[2], 1, axes, triggers));
    delete_failure = -1;
    assert(!uc_backend_delete(handles[1]) && removed[124] == 1);
    /* A cancelled pending output is removed without touching the two live ones. */
    int writer = start_identification(42, 127);
    assert(uc_backend_cancel_pending() == 2 && cancelled);
    uc_backend_poll();
    fake_ms += 300;
    uc_backend_poll();
    close(writer);
    assert(!phase && removed[127] == 1 && !removed[125] && !removed[126]);
    /* Cancellation after identification but before the core retries create
     * must remove that unadopted output, not leak it or delete another user. */
    writer = start_identification(42, 128);
    uc_backend_poll();
    fake_ms += 300;
    uc_backend_poll();
    close(writer);
    assert(phase == 4 && !removed[128]);
    assert(uc_backend_cancel_pending() == 0 && !phase && removed[128] == 1);
    assert(!removed[125] && !removed[126]);
    uc_backend_shutdown();
    assert(removed[125] == 1 && removed[126] == 1);
    assert(uc_backend_submit(handles[2], 1, axes, triggers) < 0);
}
static void power_guards(void)
{
    prepare();
    uint64_t handle = 99;
    power_status = -1;
    assert(uc_backend_create(42, &handle) < 0 && !handle && !phase);
    power_status = 1;
    assert(uc_backend_create(42, &handle) < 0 && !phase);
    power_status = 0;
    working_since = fake_ms;
    assert(uc_backend_create(42, &handle) == 2 && !phase);
    power_bits = 500;
    uc_backend_poll();
    assert(!working_since && power_status == 1);
    assert(uc_backend_suspend() == 1 && uc_backend_suspend() == 0);
    power_bits = 1000;
    uc_backend_poll();
    assert(working_since && !phase);
    logged_in = 0;
    working_since = fake_ms - 6000;
    assert(uc_backend_create(42, &handle) < 0 && !phase);
    logged_in = 1;
    fake_ms += 3000;
    uc_backend_poll();
    assert(uc_backend_suspend() == 1 && !phase);
    assert(working_since == fake_ms - 1); /* Event-loop gap restarts stable-power grace. */
}
int main(void)
{
    candidate(
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x7b][type:1][subType:22][capabilityBattery:1]");
    assert(!matches);
    isolation_and_rumble();
    power_guards();
    return 0;
}
