#ifndef UC_BACKEND_H
#define UC_BACKEND_H
#include <stdint.h>
/* Single-threaded calls from the runtime event loop. No borrowed pointer retained.
 * 0=ready/success, 2=pending, negative=failure. create is idempotent for same user.
 * Up to four independently owned outputs, one per signed-in user.
 * Identification is serialized. Delete(0) cancels pending creation only. */
int uc_backend_init(void);
/* Boolean gate; remains true for a healthy owned output. No hardware is opened. */
int uc_backend_can_start(int32_t user);
int uc_backend_create(int32_t user, uint64_t *handle);
int uc_backend_submit(uint64_t handle, uint32_t buttons, const int16_t axes[4],
                      const uint16_t triggers[2]);
int uc_backend_delete(uint64_t handle);
/* 1=sample, 0=unavailable/throttled, negative=invalid handle or first SDK error.
 * Classic motor feedback only; each owned device is polled at most every 16ms. */
int uc_backend_read_rumble(uint64_t handle, uint64_t now_ms, uint8_t *strong, uint8_t *weak);
int uc_backend_cancel_pending(void);
int uc_backend_users(int32_t users[4]);
void uc_backend_poll(void);
/* Returns 1 once for suspend/rest/long event-loop gap; runtime must disable session. */
int uc_backend_suspend(void);
/* Last nonblocking system power observation: -1 unknown, 0 working, 1 rest. */
int uc_backend_power_state(void);
void uc_backend_shutdown(void);
#endif
