#include "uc_core.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern int uc_backend_create(int32_t, uint64_t *);
extern int uc_backend_submit(uint64_t, uint32_t, const int16_t[4], const uint16_t[2]);
extern int uc_backend_delete(uint64_t);
extern int uc_backend_read_rumble(uint64_t, uint64_t, uint8_t *, uint8_t *);
#define LOG_COUNT 128u
struct log_entry {
    uint64_t timestamp, generation, session;
    uint8_t level;
    const char *code;
};
struct core_session {
    uint64_t id, handle, last_input, sequence;
    int32_t user;
    int physical, have_sequence, neutral, closing, pending;
};
static struct {
    struct core_session sessions[UC_CORE_MAX_SESSIONS];
    uint64_t serial, clock, generation, dropped;
    int suspended;
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
static int fail_session(const char *code, uint64_t session)
{
    log_event(UC_LOG_ERROR, code, session);
    return -1;
}
static int fail(const char *code)
{
    return fail_session(code, 0);
}
static int observe(uint64_t now)
{
    if (now < state.clock)
        return fail("clock_regressed");
    state.clock = now;
    return 0;
}
static struct core_session *find_session(uint64_t id)
{
    if (!id)
        return NULL;
    for (unsigned i = 0; i < UC_CORE_MAX_SESSIONS; ++i)
        if (state.sessions[i].id == id)
            return &state.sessions[i];
    return NULL;
}
static int occupied(void)
{
    for (unsigned i = 0; i < UC_CORE_MAX_SESSIONS; ++i)
        if (state.sessions[i].id)
            return 1;
    return 0;
}
static int neutralize(struct core_session *s)
{
    const int16_t axes[4] = {0};
    const uint16_t triggers[2] = {0};
    if (uc_backend_submit(s->handle, 0, axes, triggers))
        return fail_session("neutral_failed", s->id);
    s->neutral = 1;
    return 0;
}
static int remove_session(struct core_session *s)
{
    s->closing = 1;
    if (s->pending) {
        /* Backend handle zero cancels only the single serialized creation. */
        if (uc_backend_delete(0))
            return fail_session("cancel_failed", s->id);
        log_event(UC_LOG_INFO, "pending_cancelled", s->id);
    } else {
        /* Rest may invalidate input writes. Still attempt owned removal. */
        if (!s->neutral)
            (void)neutralize(s);
        if (uc_backend_delete(s->handle))
            return fail_session("delete_failed", s->id);
        log_event(UC_LOG_INFO, "disabled", s->id);
    }
    memset(s, 0, sizeof *s);
    return 0;
}
static int enable(int32_t user, uint64_t now, uint64_t *session, int physical)
{
    if (!session)
        return fail("invalid_session_output");
    uint64_t retry_token = physical ? *session : 0;
    *session = 0;
    if (observe(now))
        return -1;
    if (user <= 0)
        return fail("invalid_user");
    struct core_session *candidate = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < UC_CORE_MAX_SESSIONS; ++i) {
        struct core_session *s = &state.sessions[i];
        if (!s->id) {
            if (!free_slot)
                free_slot = s;
            continue;
        }
        if (s->pending && s->user == user && s->physical == physical && !s->closing &&
            (!physical || retry_token == s->id))
            candidate = s;
        else if (!physical || !s->physical || s->user == user || s->pending || state.suspended) {
            log_event(UC_LOG_TRACE, "enable_busy", s->id);
            return 2;
        }
    }
    if (!candidate) {
        if (physical && retry_token)
            return fail_session("stale_creation_retry", retry_token);
        if (!free_slot)
            return 2;
        if (state.serial == UINT64_MAX)
            return fail("session_exhausted");
        candidate = free_slot;
        *candidate = (struct core_session){
            .id = state.serial++, .user = user, .physical = physical, .pending = 1};
    }
    uint64_t handle = 0;
    int result = uc_backend_create(user, &handle);
    if (result == 2) {
        if (physical)
            *session = candidate->id;
        return 2;
    }
    if (result) {
        /* A backend may still own a failed partially created device. Reserve
         * this user and block new creation until explicit cancellation succeeds. */
        candidate->closing = 1;
        if (physical)
            *session = candidate->id;
        return fail_session("create_failed", candidate->id);
    }
    candidate->pending = 0;
    candidate->handle = handle;
    candidate->closing = 1;
    candidate->last_input = now;
    if (neutralize(candidate)) {
        (void)remove_session(candidate);
        if (physical && candidate->id)
            *session = candidate->id;
        return -1;
    }
    candidate->closing = 0;
    if (state.suspended)
        log_event(UC_LOG_INFO, "resumed", candidate->id);
    state.suspended = 0;
    *session = candidate->id;
    log_event(UC_LOG_INFO, "enabled", candidate->id);
    return 0;
}
int uc_core_enable(int32_t user, uint64_t now, uint64_t *session)
{
    return enable(user, now, session, 0);
}
int uc_core_enable_physical(int32_t user, uint64_t now, uint64_t *session)
{
    return enable(user, now, session, 1);
}
int uc_core_submit(uint64_t session, uint64_t seq, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now)
{
    if (observe(now))
        return -1;
    struct core_session *s = find_session(session);
    if (!s || (s->have_sequence && seq <= s->sequence)) {
        log_event(UC_LOG_WARN, "stale_input", session);
        return -1;
    }
    if (s->closing || s->pending || state.suspended || !axes || !triggers)
        return fail_session("input_rejected", session);
    if (state.swap) {
        uint32_t a = buttons & (UINT32_C(1) << 14), b = buttons & (UINT32_C(1) << 13);
        buttons = (buttons & ~((UINT32_C(1) << 14) | (UINT32_C(1) << 13))) | (a >> 1) | (b << 1);
    }
    uint32_t mapped = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (buttons & (UINT32_C(1) << i))
            mapped |= UINT32_C(1) << (state.configured ? state.mapping[i] : i);
    if (uc_backend_submit(s->handle, mapped, axes, triggers))
        return fail_session("submit_failed", session);
    s->sequence = seq;
    s->have_sequence = 1;
    s->last_input = now;
    s->neutral =
        !mapped && !axes[0] && !axes[1] && !axes[2] && !axes[3] && !triggers[0] && !triggers[1];
    log_event(UC_LOG_TRACE, "input_accepted", session);
    return 0;
}
int uc_core_disable_session(uint64_t session)
{
    struct core_session *s = find_session(session);
    if (!s) {
        (void)fail_session("stale_session", session);
        return -2;
    }
    return remove_session(s);
}
int uc_core_disable(void)
{
    int result = 0;
    for (unsigned i = 0; i < UC_CORE_MAX_SESSIONS; ++i)
        if (state.sessions[i].id && remove_session(&state.sessions[i]))
            result = -1;
    return result;
}
int uc_core_refresh_source(uint64_t session, uint64_t now)
{
    if (observe(now))
        return -1;
    struct core_session *s = find_session(session);
    if (!s || !s->have_sequence || s->closing || s->pending || state.suspended)
        return fail_session("source_lease_rejected", session);
    if (now - s->last_input >= 500) {
        if (!s->neutral && neutralize(s))
            return -1;
        return fail_session("source_lease_expired", session);
    }
    s->last_input = now;
    return 0;
}
int uc_core_tick(uint64_t now)
{
    if (observe(now))
        return -1;
    int result = 0;
    for (unsigned i = 0; i < UC_CORE_MAX_SESSIONS; ++i) {
        struct core_session *s = &state.sessions[i];
        if (!s->id)
            continue;
        if (s->closing) {
            if (remove_session(s))
                result = -1;
            continue;
        }
        if (!s->pending && !s->neutral && now - s->last_input >= 500) {
            if (neutralize(s))
                result = -1;
            else
                log_event(UC_LOG_INFO, "released", s->id);
        }
    }
    return result;
}
int uc_core_suspend(void)
{
    if (!state.suspended) {
        if (state.generation == UINT64_MAX)
            return fail("generation_exhausted");
        ++state.generation;
        state.suspended = 1;
        log_event(UC_LOG_INFO, "suspended", 0);
    }
    return uc_core_disable();
}
uint64_t uc_core_active_session(void)
{
    for (unsigned i = 0; i < UC_CORE_MAX_SESSIONS; ++i)
        if (state.sessions[i].id && !state.sessions[i].pending)
            return state.sessions[i].id;
    return 0;
}
int uc_core_session_active(uint64_t session)
{
    const struct core_session *s = find_session(session);
    return s && !s->pending && !s->closing && !state.suspended;
}
int uc_core_read_rumble(uint64_t session, uint64_t now, uint8_t *strong, uint8_t *weak)
{
    if (strong)
        *strong = 0;
    if (weak)
        *weak = 0;
    if (!strong || !weak || observe(now))
        return -1;
    const struct core_session *s = find_session(session);
    if (!s)
        return fail_session("stale_rumble", session);
    if (!uc_core_session_active(session))
        return 0;
    int result = uc_backend_read_rumble(s->handle, now, strong, weak);
    if (result != 1) {
        *strong = 0;
        *weak = 0;
    }
    if (result < 0)
        return fail_session("rumble_failed", session);
    return result;
}
int uc_core_settings(uint8_t recording, uint8_t swap, const uint8_t mapping[32])
{
    if (!mapping || recording > UC_LOG_TRACE || swap > 1)
        return fail("invalid_settings");
    for (unsigned i = 0; i < 32; ++i)
        if (mapping[i] > 31)
            return fail("invalid_mapping");
    if (occupied())
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
