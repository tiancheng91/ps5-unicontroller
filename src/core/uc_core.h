#ifndef UC_CORE_H
#define UC_CORE_H
#include <stddef.h>
#include <stdint.h>
/* Single event-loop owner. No pointers are retained. 0=success, 2=busy/pending,
 * negative=failure. Construction does not touch hardware. */
enum uc_log_level {
    UC_LOG_OFF,
    UC_LOG_ERROR,
    UC_LOG_WARN,
    UC_LOG_INFO,
    UC_LOG_DEBUG,
    UC_LOG_TRACE
};
int uc_core_enable(int32_t user, uint64_t now_ms, uint64_t *session);
int uc_core_submit(uint64_t session, uint64_t sequence, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now_ms);
/* Trusted physical adapter only: renews the 500ms output lease after a fresh
 * successful transport-health check. Requires an accepted input in this session;
 * never changes sequence/state/frame counts or resurrects an expired hold.
 * Not exposed through HTTP. */
int uc_core_refresh_source(uint64_t session, uint64_t now_ms);
int uc_core_tick(uint64_t now_ms);
/* Cleanup is neutral-before-delete and retryable; failures retain ownership. */
int uc_core_disable(void);
int uc_core_suspend(void);
uint64_t uc_core_active_session(void);
/* Validated and applied atomically only when inactive; Cross/Circle swap is
 * applied before the source->destination map. Recording level defaults to Info. */
int uc_core_settings(uint8_t recording_level, uint8_t swap_ab, const uint8_t mapping[32]);
/* JSON entries+dropped. Page filter never changes recording level. Returns
 * bytes excluding NUL, or zero (empty output) for invalid level/small buffer.
 * Lifecycle timestamps use the last supplied clock. No payload/user names log. */
size_t uc_core_logs(char *buffer, size_t capacity, uint8_t page_level);
#endif
