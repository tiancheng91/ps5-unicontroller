/* Host-only lifecycle mock: no PS5 devices or synthetic game feedback. */
#include "../../src/ps5/uc_backend.h"
#include <string.h>
static struct {
    uint64_t handle;
    int32_t user;
} outputs[4];
static uint64_t next_id = 1;
static int find_output(uint64_t handle)
{
    if (handle)
        for (int i = 0; i < 4; i++)
            if (outputs[i].handle == handle)
                return i;
    return -1;
}
int uc_backend_init(void)
{
    return 0;
}
int uc_backend_create(int32_t user, uint64_t *handle)
{
    if (!handle || user < 0 || user > 4 || !next_id)
        return -1;
    *handle = 0;
    for (int i = 0; i < 4; i++) {
        if (!outputs[i].handle) {
            outputs[i].handle = next_id++;
            outputs[i].user = user;
            *handle = outputs[i].handle;
            return 0;
        }
    }
    return -1;
}
int uc_backend_submit(uint64_t handle, uint32_t buttons, const int16_t axes[4],
                      const uint16_t triggers[2])
{
    (void)buttons;
    return axes && triggers && find_output(handle) >= 0 ? 0 : -1;
}
int uc_backend_delete(uint64_t handle)
{
    if (!handle)
        return 0; /* This synchronous mock has no pending creation. */
    int index = find_output(handle);
    if (index < 0)
        return -1;
    memset(&outputs[index], 0, sizeof(outputs[index]));
    return 0;
}
int uc_backend_cancel_pending(void)
{
    return 0;
}
int uc_backend_users(int32_t ids[4])
{
    if (!ids)
        return -1;
    for (int i = 0; i < 4; i++)
        ids[i] = i + 1;
    return 0;
}
void uc_backend_poll(void)
{
}
int uc_backend_suspend(void)
{
    return 0;
}
void uc_backend_shutdown(void)
{
    memset(outputs, 0, sizeof(outputs));
}
int uc_backend_can_start(int32_t user)
{
    return user >= 1 && user <= 4;
}
int uc_backend_read_rumble(uint64_t handle, uint64_t now, uint8_t *strong, uint8_t *weak)
{
    (void)now;
    if (!strong || !weak || find_output(handle) < 0)
        return -1;
    *strong = *weak = 0;
    return 0; /* No game/backend exists on the host: do not invent samples. */
}

int uc_backend_power_state(void)
{
    return 0;
}

void uc_backend_auto_watch(int enabled)
{
    (void)enabled;
}
int uc_backend_auto_due(int32_t user, int idle)
{
    (void)user;
    (void)idle;
    return 0; /* A host cannot manufacture PS5 desktop events. */
}

int uc_backend_transport_ready(void)
{
    return 1;
}
