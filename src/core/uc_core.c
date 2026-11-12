#include "uc_core.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern int uc_backend_create(int32_t, uint64_t *);
extern int uc_backend_submit(uint64_t, uint32_t, const int16_t[4], const uint16_t[2]);
extern int uc_backend_delete(uint64_t);
#define LOG_COUNT 128u
struct log_entry {
    uint64_t timestamp, generation, session;
    uint8_t level;
    const char *code;
};
static struct {
    uint64_t session, serial, handle, last_input, clock, generation;
    uint64_t sequence, dropped;
    int have_sequence, neutral, closing, pending, suspended;
    int32_t pending_user;
    uint8_t recording, swap, mapping[32];
    int configured;
    struct log_entry logs[LOG_COUNT];
    unsigned log_start, log_count;
} state = {.serial = 1, .recording = UC_LOG_INFO};
static void log_event(uint8_t level, const char *code, uint64_t session)
{
    if (!state.recording || level > state.recording)
        return;
    unsigned index;
    if (state.log_count == LOG_COUNT) {
        index = state.log_start;
        state.log_start = (state.log_start + 1) % LOG_COUNT;
        if (state.dropped != UINT64_MAX)
            ++state.dropped;
    } else
        index = (state.log_start + state.log_count++) % LOG_COUNT;
    state.logs[index] = (struct log_entry){state.clock, state.generation, session, level, code};
}
static int fail(const char *code)
{
    log_event(UC_LOG_ERROR, code, state.session);
    return -1;
}
static int observe(uint64_t now)
{
    if (now < state.clock)
        return fail("clock_regressed");
    state.clock = now;
    return 0;
}
static int neutralize(void)
{
    const int16_t axes[4] = {0};
    const uint16_t triggers[2] = {0};
    if (uc_backend_submit(state.handle, 0, axes, triggers))
        return fail("neutral_failed");
    state.neutral = 1;
    return 0;
}
int uc_core_enable(int32_t user, uint64_t now, uint64_t *session)
{
    if (!session)
        return fail("invalid_session_output");
    *session = 0;
    if (observe(now))
        return -1;
    if (user <= 0)
        return fail("invalid_user");
    if (state.session || state.closing || (state.pending && state.pending_user != user)) {
        log_event(UC_LOG_WARN, "enable_busy", state.session);
        return 2;
    }
    if (state.serial == UINT64_MAX)
        return fail("session_exhausted");
    uint64_t handle = 0;
    int result = uc_backend_create(user, &handle);
    if (result == 2) {
        state.pending = 1;
        state.pending_user = user;
        return 2;
    }
    if (result)
        return fail("create_failed");
    state.pending = 0;
    state.handle = handle;
    state.session = state.serial++;
    state.closing = 1;
    state.neutral = 0;
    state.have_sequence = 0;
    state.last_input = now;
    if (neutralize()) {
        (void)uc_core_disable();
        return -1;
    }
    state.closing = 0;
    if (state.suspended)
        log_event(UC_LOG_INFO, "resumed", state.session);
    state.suspended = 0;
    *session = state.session;
    log_event(UC_LOG_INFO, "enabled", state.session);
    return 0;
}
int uc_core_submit(uint64_t session, uint64_t seq, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now)
{
    if (observe(now))
        return -1;
    if (!session || session != state.session || (state.have_sequence && seq <= state.sequence)) {
        log_event(UC_LOG_WARN, "stale_input", session);
        return -1;
    }
    if (state.closing || state.suspended || !axes || !triggers)
        return fail("input_rejected");
    if (state.swap) {
        uint32_t a = buttons & (UINT32_C(1) << 14), b = buttons & (UINT32_C(1) << 13);
        buttons = (buttons & ~((UINT32_C(1) << 14) | (UINT32_C(1) << 13))) | (a >> 1) | (b << 1);
    }
    uint32_t mapped = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (buttons & (UINT32_C(1) << i))
            mapped |= UINT32_C(1) << (state.configured ? state.mapping[i] : i);
    if (uc_backend_submit(state.handle, mapped, axes, triggers))
        return fail("submit_failed");
    state.sequence = seq;
    state.have_sequence = 1;
    state.last_input = now;
    state.neutral =
        !mapped && !axes[0] && !axes[1] && !axes[2] && !axes[3] && !triggers[0] && !triggers[1];
    log_event(UC_LOG_TRACE, "input_accepted", session);
    return 0;
}
int uc_core_disable(void)
{
    if (state.pending) {
        state.closing = 1;
        if (uc_backend_delete(0))
            return fail("cancel_failed");
        state.pending = 0;
        state.closing = 0;
        log_event(UC_LOG_INFO, "pending_cancelled", 0);
    }
    if (!state.session)
        return 0;
    state.closing = 1;
    /* Rest may invalidate input submission before deletion. Always attempt
     * removal of our owned device even when the final neutral frame fails. */
    if (!state.neutral)
        (void)neutralize();
    if (uc_backend_delete(state.handle))
        return fail("delete_failed");
    log_event(UC_LOG_INFO, "disabled", state.session);
    state.session = 0;
    state.handle = 0;
    state.closing = 0;
    return 0;
}
int uc_core_refresh_source(uint64_t session, uint64_t now)
{
    if (observe(now))
        return -1;
    if (!session || session != state.session || !state.have_sequence || state.closing ||
        state.suspended)
        return fail("source_lease_rejected");
    if (now - state.last_input >= 500) {
        if (!state.neutral && neutralize())
            return -1;
        return fail("source_lease_expired");
    }
    state.last_input = now;
    return 0;
}
int uc_core_tick(uint64_t now)
{
    if (observe(now))
        return -1;
    if (state.closing)
        return uc_core_disable();
    if (state.session && !state.neutral && now - state.last_input >= 500) {
        if (neutralize())
            return -1;
        log_event(UC_LOG_INFO, "released", state.session);
    }
    return 0;
}
int uc_core_suspend(void)
{
    if (!state.suspended) {
        if (state.generation == UINT64_MAX)
            return fail("generation_exhausted");
        ++state.generation;
        state.suspended = 1;
        log_event(UC_LOG_INFO, "suspended", state.session);
    }
    return uc_core_disable();
}
uint64_t uc_core_active_session(void)
{
    return state.session;
}
int uc_core_settings(uint8_t recording, uint8_t swap, const uint8_t mapping[32])
{
    if (!mapping || recording > UC_LOG_TRACE || swap > 1)
        return fail("invalid_settings");
    for (unsigned i = 0; i < 32; ++i)
        if (mapping[i] > 31)
            return fail("invalid_mapping");
    if (state.session || state.pending || state.closing)
        return 2;
    state.recording = recording;
    state.swap = swap;
    memcpy(state.mapping, mapping, 32);
    state.configured = 1;
    log_event(UC_LOG_INFO, "settings_applied", 0);
    return 0;
}
static int append(char *buffer, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap)
        return -1;
    va_list args;
    va_start(args, fmt);
    int count = vsnprintf(buffer + *used, cap - *used, fmt, args);
    va_end(args);
    if (count < 0 || (size_t)count >= cap - *used)
        return -1;
    *used += (size_t)count;
    return 0;
}
size_t uc_core_logs(char *buffer, size_t capacity, uint8_t level)
{
    static const char *levels[] = {"off", "error", "warn", "info", "debug", "trace"};
    size_t used = 0;
    unsigned emitted = 0;
    if (!buffer || !capacity)
        return 0;
    buffer[0] = 0;
    if (level > UC_LOG_TRACE)
        return 0;
    if (append(buffer, capacity, &used, "{\"entries\":["))
        goto small;
    for (unsigned i = 0; i < state.log_count; ++i) {
        const struct log_entry *e = &state.logs[(state.log_start + i) % LOG_COUNT];
        if (!level || e->level > level)
            continue;
        if (append(buffer, capacity, &used,
                   "%s{\"level\":\"%s\",\"code\":\"%s\",\"timestamp_ms\":%llu,\"generation\":%llu,"
                   "\"session_id\":%llu}",
                   emitted ? "," : "", levels[e->level], e->code, (unsigned long long)e->timestamp,
                   (unsigned long long)e->generation, (unsigned long long)e->session))
            goto small;
        ++emitted;
    }
    if (append(buffer, capacity, &used, "],\"dropped\":%llu}", (unsigned long long)state.dropped))
        goto small;
    return used;
small:
    buffer[0] = 0;
    return 0;
}
