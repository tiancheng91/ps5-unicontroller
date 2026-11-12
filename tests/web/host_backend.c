/* Host-only backend: records lifecycle in memory; does not emulate actual PS5 output. */
#include "../../src/ps5/uc_backend.h"
static uint64_t owned, next_id = 1;
int uc_backend_init(void)
{
    return 0;
}
int uc_backend_create(int32_t user, uint64_t *handle)
{
    if (user != 1 || owned)
        return -1;
    *handle = owned = next_id++;
    return 0;
}
int uc_backend_submit(uint64_t handle, uint32_t buttons, const int16_t axes[4],
                      const uint16_t triggers[2])
{
    (void)buttons;
    (void)axes;
    (void)triggers;
    return handle && handle == owned ? 0 : -1;
}
int uc_backend_delete(uint64_t handle)
{
    if (handle != owned)
        return -1;
    owned = 0;
    return 0;
}
int uc_backend_cancel_pending(void)
{
    return 0;
}
int uc_backend_users(int32_t ids[4])
{
    ids[0] = 1;
    ids[1] = ids[2] = ids[3] = -1;
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
    owned = 0;
}

int uc_backend_can_start(int32_t user)
{
    return user == 1;
}
