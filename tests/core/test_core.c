#include "uc_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int write_failure, delete_failure, create_pending, create_failure;
static unsigned writes, deletes, create_calls, cancel_calls;
static uint64_t fail_write_handle, fail_delete_handle, rumble_handle;
static uint32_t buttons_by_handle[4];
static unsigned deletes_by_handle[4];
static int rumble_result = 1;
static uint32_t last_buttons;
static int16_t last_axes[4];
static uint16_t last_triggers[2];
int uc_backend_create(int32_t user, uint64_t *handle)
{
    assert(user >= 0);
    ++create_calls;
    if (create_failure)
        return -1;
    if (create_pending)
        return 2;
    assert(user <= 4);
    *handle = user == 0 ? 99 : 98 + (uint64_t)user;
    return 0;
}
int uc_backend_submit(uint64_t handle, uint32_t buttons, const int16_t axes[4],
                      const uint16_t triggers[2])
{
    assert(handle >= 99 && handle <= 102);
    if (write_failure || handle == fail_write_handle)
        return -1;
    ++writes;
    buttons_by_handle[handle - 99] = buttons;
    last_buttons = buttons;
    memcpy(last_axes, axes, sizeof last_axes);
    memcpy(last_triggers, triggers, sizeof last_triggers);
    return 0;
}
int uc_backend_delete(uint64_t handle)
{
    assert((handle >= 99 && handle <= 102) || handle == 0);
    if (delete_failure || (fail_delete_handle && handle == fail_delete_handle))
        return -1;
    ++deletes;
    if (handle)
        ++deletes_by_handle[handle - 99];
    else
        ++cancel_calls;
    return 0;
}
int uc_backend_read_rumble(uint64_t handle, uint64_t now, uint8_t *strong, uint8_t *weak)
{
    assert(now >= 6000);
    rumble_handle = handle;
    *strong = (uint8_t)handle;
    *weak = (uint8_t)(handle + 1);
    return rumble_result;
}
static void test_multiple_sessions(void);
int main(void)
{
    const int16_t axes[4] = {123, -456, 0, 0}, zero[4] = {0};
    const uint16_t triggers[2] = {0, 44}, tz[2] = {0};
    uint8_t mapping[32];
    for (unsigned i = 0; i < 32; ++i)
        mapping[i] = (uint8_t)i;
    uint64_t session = 0, next = 0;
    char logs[32768];
    assert(uc_core_active_session() == 0 && writes == 0);
    assert(uc_core_enable(1, 10, &session) == 0 && session);
    assert(last_buttons == 0);
    assert(uc_core_settings(3, 0, mapping) == 0);
    assert(uc_core_submit(session, 0, 16384, axes, triggers, 20) == 0);
    assert(last_axes[0] == 123 && last_triggers[1] == 44);
    assert(uc_core_submit(session, 0, 0, zero, tz, 21) < 0);
    assert(uc_core_tick(519) == 0 && last_buttons == 16384);
    write_failure = 1;
    assert(uc_core_tick(520) < 0 && last_buttons == 16384);
    write_failure = 0;
    assert(uc_core_tick(521) == 0 && last_buttons == 0);
    unsigned count = writes;
    assert(uc_core_tick(522) == 0 && writes == count);
    write_failure = 1;
    assert(uc_core_submit(session, 1, 1, zero, tz, 523) < 0);
    write_failure = 0;
    assert(uc_core_submit(session, 1, 1, zero, tz, 524) == 0);
    write_failure = 1;
    delete_failure = 1;
    assert(uc_core_disable() < 0 && uc_core_active_session() == session && deletes == 0);
    assert(uc_core_submit(session, 2, 1, zero, tz, 525) < 0);
    write_failure = 0;
    delete_failure = 1;
    assert(uc_core_disable() < 0);
    count = writes;
    delete_failure = 0;
    assert(uc_core_disable() == 0 && writes == count);
    assert(uc_core_enable(1, 530, &next) == 0 && next != session);
    assert(uc_core_submit(session, 100, 1, zero, tz, 531) < 0);
    assert(uc_core_submit(next, 1, 1, zero, tz, 529) < 0);
    assert(uc_core_suspend() == 0 && uc_core_active_session() == 0);
    assert(uc_core_tick(1000) == 0 && uc_core_active_session() == 0);
    mapping[14] = 5;
    mapping[13] = 7;
    mapping[15] = 8;
    mapping[12] = 9;
    assert(uc_core_settings(UC_LOG_TRACE, 1, mapping) == 0);
    assert(uc_core_enable(1, 1000, &next) == 0);
    assert(uc_core_submit(next, 1, 16384, zero, tz, 1001) == 0 && last_buttons == 128);
    assert(uc_core_submit(next, 2, 8192, zero, tz, 1002) == 0 && last_buttons == 32);
    assert(uc_core_submit(next, 3, 1u << 12, zero, tz, 1003) == 0 && last_buttons == (1u << 8));
    assert(uc_core_submit(next, 4, 1u << 15, zero, tz, 1004) == 0 && last_buttons == (1u << 9));
    assert(uc_core_submit(next, 5, 0xF000u | (1u << 16) | 1u, zero, tz, 1005) == 0 &&
           last_buttons == ((1u << 5) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 16) | 1u));
    for (unsigned i = 6; i < 150; ++i)
        assert(uc_core_submit(next, i, 0, zero, tz, 1000 + i) == 0);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_TRACE) > 0);
    assert(strstr(logs, "input_accepted") && strstr(logs, "\"dropped\":0}") == NULL);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_INFO) > 0 &&
           strstr(logs, "input_accepted") == NULL);
    assert(uc_core_logs(logs, 8, UC_LOG_TRACE) == 0 && logs[0] == 0);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_OFF) > 0 && strstr(logs, "\"entries\":[]"));
    assert(uc_core_disable() == 0);
    /* Physical adapters have already applied their own device layout. Global
     * test-controller swapping must not flip it a second time before mapping. */
    next = 0;
    assert(uc_core_enable_physical(1, 1150, &next) == 0);
    assert(uc_core_submit(next, 1, 1u << 13, zero, tz, 1151) == 0 && last_buttons == (1u << 7));
    assert(uc_core_disable() == 0);
    assert(uc_core_settings(UC_LOG_OFF, 0, mapping) == 0);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_TRACE) > 0);
    char before[32768];
    strcpy(before, logs);
    assert(uc_core_enable(-1, 1200, &next) < 0);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_TRACE) > 0 && !strcmp(before, logs));
    mapping[0] = 32;
    assert(uc_core_settings(UC_LOG_INFO, 0, mapping) < 0);
    mapping[0] = 0;
    create_pending = 1;
    assert(uc_core_enable(1, 1200, &next) == 2 && next == 0);
    assert(uc_core_settings(UC_LOG_INFO, 0, mapping) == 0); /* Live/pending logging only. */
    assert(uc_core_settings_check(1, mapping) == 0);
    assert(uc_core_enable(2, 1201, &next) == 2);
    delete_failure = 1;
    assert(uc_core_suspend() < 0);
    delete_failure = 0;
    assert(uc_core_tick(1202) == 0);
    create_pending = 0;
    assert(uc_core_enable(1, 1203, &next) == 0);
    unsigned settings_writes = writes;
    assert(uc_core_settings(UC_LOG_WARN, 0, mapping) == 0);
    assert(uc_core_session_active(next) && writes == settings_writes);
    assert(uc_core_settings(UC_LOG_WARN, 1, mapping) == 0);
    assert(uc_core_session_active(next) && writes == settings_writes);
    assert(uc_core_submit(next, 1, 1u << 14, zero, tz, 1204) == 0 &&
           last_buttons == (1u << mapping[13]));
    assert(uc_core_settings(UC_LOG_WARN, 0, mapping) == 0);
    assert(uc_core_submit(next, 2, 0, zero, tz, 1204) == 0 && last_buttons == 0);
    assert(uc_core_disable() == 0);
    write_failure = 1;
    delete_failure = 1;
    assert(uc_core_enable(1, 1204, &next) < 0 && uc_core_active_session() != 0);
    delete_failure = 0;
    assert(uc_core_suspend() == 0 && uc_core_active_session() == 0); /* SDK input already gone. */
    write_failure = 0;
    assert(uc_core_disable() == 0);
    /* A trusted, healthy physical source can hold without fabricating frames. */
    assert(uc_core_enable(1, 2000, &next) == 0);
    assert(uc_core_refresh_source(next, 2001) < 0); /* No accepted input yet. */
    assert(uc_core_submit(next, 1, 1, zero, tz, 2002) == 0);
    count = writes;
    for (uint64_t now = 2100; now <= 4000; now += 100) {
        assert(uc_core_refresh_source(next, now) == 0);
        assert(uc_core_tick(now) == 0 && last_buttons == 1 && writes == count);
    }
    assert(uc_core_refresh_source(next + 1, 4001) < 0);
    assert(uc_core_submit(next, 1, 0, zero, tz, 4002) < 0); /* Sequence unchanged. */
    assert(uc_core_refresh_source(next, 4500) < 0 && last_buttons == 0);
    assert(uc_core_refresh_source(next, 4501) < 0); /* Never resurrect a timed-out hold. */
    assert(uc_core_disable() == 0);
    assert(uc_core_refresh_source(next, 4502) < 0);
    assert(uc_core_enable(1, 4503, &next) == 0);
    assert(uc_core_submit(next, 1, 1, zero, tz, 4504) == 0);
    assert(uc_core_tick(5004) == 0 && last_buttons == 0); /* Phone still expires. */
    assert(uc_core_suspend() == 0);
    assert(uc_core_refresh_source(next, 5005) < 0);
    test_multiple_sessions();
    uint64_t system_session = 0, other = 0;
    assert(uc_core_enable(0, 8200, &other) < 0);
    assert(uc_core_enable_physical(0, 8200, &other) < 0);
    assert(uc_core_enable_system_assigned(8201, &system_session) == 0);
    assert(uc_core_enable_physical(2, 8202, &other) == 2 && other == 0);
    assert(uc_core_enable(2, 8202, &other) == 2);
    assert(uc_core_enable_system_assigned(8202, &other) == 2 && other == 0);
    assert(uc_core_submit(system_session, 1, 1u << 16, zero, tz, 8203) == 0);
    assert(last_buttons == (1u << 16));
    assert(uc_core_tick(8703) == 0 && last_buttons == 0);
    assert(uc_core_suspend() == 0 && !uc_core_session_active(system_session));
    assert(uc_core_submit(system_session, 2, 1, zero, tz, 8704) < 0);
    other = 0;
    assert(uc_core_enable_physical(1, 8800, &other) == 0);
    system_session = 0;
    assert(uc_core_enable_system_assigned(8801, &system_session) == 2 && !system_session);
    assert(uc_core_disable() == 0);
    create_pending = 1;
    assert(uc_core_enable_system_assigned(8900, &system_session) == 2 && system_session);
    other = 0;
    assert(uc_core_enable_system_assigned(8901, &other) == 2 && !other);
    create_pending = 0;
    assert(uc_core_enable_system_assigned(8902, &system_session) == 0);
    assert(uc_core_disable_session(system_session) == 0);

    puts("core: cleanup, stale, timeout, mapping, lifecycle, pending, logs PASS");
    return 0;
}

static void test_multiple_sessions(void)
{
    const int16_t axes[4] = {0};
    const uint16_t triggers[2] = {0};
    uint8_t mapping[32];
    for (unsigned i = 0; i < 32; ++i)
        mapping[i] = (uint8_t)i;
    assert(uc_core_settings(UC_LOG_INFO, 0, mapping) == 0);
    uint64_t ids[4] = {0}, duplicate = 0;
    for (unsigned i = 0; i < 4; ++i)
        assert(uc_core_enable_physical((int32_t)i + 1, 6000, &ids[i]) == 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(uc_core_session_active(ids[i]));
    unsigned calls = create_calls;
    assert(uc_core_enable_physical(1, 6001, &duplicate) == 2 && duplicate == 0);
    assert(uc_core_enable_physical(5, 6001, &duplicate) == 2 && create_calls == calls);
    assert(uc_core_enable(1, 6001, &duplicate) == 2);
    assert(uc_core_submit(ids[0], 5, 1, axes, triggers, 6010) == 0);
    assert(uc_core_submit(ids[1], 1, 2, axes, triggers, 6200) == 0);
    assert(uc_core_submit(ids[0], 5, 0, axes, triggers, 6201) < 0);
    assert(uc_core_submit(ids[1], 2, 4, axes, triggers, 6201) == 0);
    assert(uc_core_tick(6510) == 0 && buttons_by_handle[0] == 0 && buttons_by_handle[1] == 4);
    uint8_t strong = 0, weak = 0;
    assert(uc_core_read_rumble(ids[1], 6511, &strong, &weak) == 1 && rumble_handle == 100 &&
           strong == 100 && weak == 101);
    rumble_result = 0;
    assert(uc_core_read_rumble(ids[0], 6512, &strong, &weak) == 0 && strong == 0 && weak == 0);
    rumble_result = -1;
    assert(uc_core_read_rumble(ids[0], 6513, &strong, &weak) < 0 && strong == 0 && weak == 0);
    rumble_result = 1;
    unsigned other_deleted = deletes_by_handle[1];
    assert(uc_core_disable_session(ids[0]) == 0);
    assert(!uc_core_session_active(ids[0]) && uc_core_session_active(ids[1]));
    assert(deletes_by_handle[1] == other_deleted);
    assert(uc_core_read_rumble(ids[0], 6514, &strong, &weak) < 0 && !strong && !weak);
    assert(uc_core_disable_session(ids[0]) < 0);
    uint64_t replacement = 0;
    assert(uc_core_enable_physical(1, 6515, &replacement) == 0 && replacement != ids[0]);
    assert(uc_core_submit(ids[0], 100, 1, axes, triggers, 6516) < 0);
    assert(uc_core_submit(replacement, 1, 1, axes, triggers, 6516) == 0);
    assert(uc_core_submit(ids[1], 3, 2, axes, triggers, 6517) == 0);
    fail_delete_handle = 99;
    assert(uc_core_disable_session(replacement) < 0);
    assert(!uc_core_session_active(replacement) && uc_core_session_active(ids[1]));
    fail_write_handle = 100;
    assert(uc_core_submit(ids[2], 1, 8, axes, triggers, 6518) == 0);
    assert(uc_core_tick(7018) < 0); /* One failed cleanup/write does not skip other outputs. */
    assert(buttons_by_handle[2] == 0);
    fail_write_handle = 0;
    fail_delete_handle = 0;
    assert(uc_core_tick(7019) == 0 && uc_core_session_active(ids[1]));
    assert(uc_core_disable() == 0 && uc_core_active_session() == 0);

    /* Only one backend creation may be pending; cancellation must not remove an active sibling. */
    ids[0] = 0;
    assert(uc_core_enable_physical(1, 7100, &ids[0]) == 0);
    create_pending = 1;
    ids[1] = 0;
    assert(uc_core_enable_physical(2, 7101, &ids[1]) == 2 && ids[1] != 0);
    uint64_t pending = ids[1];
    calls = create_calls;
    uint64_t foreign_retry = 0;
    assert(uc_core_enable_physical(2, 7101, &foreign_retry) == 2 && foreign_retry == 0);
    assert(create_calls == calls); /* Cannot adopt another source's same-user reservation. */
    assert(uc_core_enable_physical(3, 7102, &duplicate) == 2 && duplicate == 0 &&
           create_calls == calls);
    assert(uc_core_enable_physical(2, 7103, &ids[1]) == 2 && ids[1] == pending);
    assert(uc_core_settings(UC_LOG_INFO, 0, mapping) == 0); /* Live/pending logging only. */
    assert(uc_core_settings_check(1, mapping) == 0);
    unsigned cancellation = cancel_calls;
    other_deleted = deletes_by_handle[0];
    assert(uc_core_disable_session(pending) == 0 && cancel_calls == cancellation + 1);
    assert(deletes_by_handle[0] == other_deleted && uc_core_session_active(ids[0]));
    create_pending = 0;
    ids[1] = 0;
    assert(uc_core_enable_physical(2, 7104, &ids[1]) == 0 && ids[1] != pending);
    assert(uc_core_suspend() == 0 && !uc_core_session_active(ids[0]) &&
           !uc_core_session_active(ids[1]));
    assert(uc_core_tick(8000) == 0 && uc_core_active_session() == 0);
    assert(uc_core_enable(1, 8001, &ids[0]) == 0);
    calls = create_calls;
    assert(uc_core_enable_physical(2, 8002, &duplicate) == 2 && create_calls == calls);
    assert(uc_core_disable() == 0);
    ids[0] = 0;
    assert(uc_core_enable_physical(1, 8100, &ids[0]) == 0);
    create_failure = 1;
    ids[1] = 0;
    assert(uc_core_enable_physical(2, 8101, &ids[1]) < 0 && ids[1] != 0);
    assert(!uc_core_session_active(ids[1]) && uc_core_session_active(ids[0]));
    create_failure = 0;
    assert(uc_core_disable_session(ids[1]) == 0 && uc_core_session_active(ids[0]));
    fail_write_handle = fail_delete_handle = 100;
    ids[1] = 0;
    assert(uc_core_enable_physical(2, 8102, &ids[1]) < 0 && ids[1] != 0);
    assert(!uc_core_session_active(ids[1]) && uc_core_session_active(ids[0]));
    fail_write_handle = fail_delete_handle = 0;
    assert(uc_core_disable_session(ids[1]) == 0 && uc_core_session_active(ids[0]));
    fail_write_handle = 100;
    ids[1] = 0;
    assert(uc_core_enable_physical(2, 8103, &ids[1]) < 0 && ids[1] == 0);
    fail_write_handle = 0;
    assert(uc_core_disable() == 0);
}
