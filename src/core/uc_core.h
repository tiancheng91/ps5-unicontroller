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
#define UC_CORE_MAX_SESSIONS 4u
/* Physical outputs may coexist only for distinct users. Creation is serialized.
 * Physical *session is in/out: initialize to zero for a fresh caller. A pending
 * creation returns 2 and a reserved nonzero token; only that token may retry
 * creation for the same user. Use disable_session(token) to cancel. Errors also
 * return a nonzero token if owned cleanup remains; retain and remove that token.
 * Legacy phone enable stays exclusive and returns token zero while pending. */
int uc_core_enable_physical(int32_t user, uint64_t now_ms, uint64_t *session);
/* Experimental, exclusive output without application account binding. Only the
 * resume Bluetooth path calls this; user selection is left to the PS5 shell.
 * Uses the same reservation token, input timeout and cleanup rules. */
int uc_core_enable_system_assigned(uint64_t now_ms, uint64_t *session);
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
/* Removes only this owned token, including pending creation. Stale tokens return -2; cleanup
 * failures return -1. */
int uc_core_disable_session(uint64_t session);
int uc_core_session_active(uint64_t session);
/* 1=new sample, 0=no sample/inactive, negative=error; outputs zero without sample. */
int uc_core_read_rumble(uint64_t session, uint64_t now_ms, uint8_t *strong, uint8_t *weak);
int uc_core_suspend(void);
uint64_t uc_core_active_session(void);
/* Settings may be updated live; layout changes apply on the next input report.
 * Cross/Circle and Square/Triangle layout swapping applies to the test controller before the
 * source->destination map. Physical adapters apply their own per-device layout before calling
 * submit. Recording level defaults to Info. */
/* Preflight without applying preferences: 0=allowed, negative=invalid. */
int uc_core_settings_check(uint8_t swap_ab, const uint8_t mapping[32]);
int uc_core_settings(uint8_t recording_level, uint8_t swap_ab, const uint8_t mapping[32]);
/* JSON entries+dropped. Page filter never changes recording level. Returns
 * bytes excluding NUL, or zero (empty output) for invalid level/small buffer.
 * Lifecycle timestamps use the last supplied clock. No payload/user names log. */
size_t uc_core_logs(char *buffer, size_t capacity, uint8_t page_level);
#endif
