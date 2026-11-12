#include "uc_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int write_failure, delete_failure, create_pending;
static unsigned writes, deletes;
static uint32_t last_buttons;
static int16_t last_axes[4];
static uint16_t last_triggers[2];
int uc_backend_create(int32_t user, uint64_t *handle)
{
    assert(user > 0);
    if (create_pending)
        return 2;
    *handle = 99;
    return 0;
}
int uc_backend_submit(uint64_t handle, uint32_t buttons, const int16_t axes[4],
                      const uint16_t triggers[2])
{
    assert(handle == 99);
    if (write_failure)
        return -1;
    ++writes;
    last_buttons = buttons;
    memcpy(last_axes, axes, sizeof last_axes);
    memcpy(last_triggers, triggers, sizeof last_triggers);
    return 0;
}
int uc_backend_delete(uint64_t handle)
{
    assert(handle == 99 || handle == 0);
    if (delete_failure)
        return -1;
    ++deletes;
    return 0;
}
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
    assert(uc_core_settings(3, 0, mapping) == 2);
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
    assert(uc_core_settings(UC_LOG_TRACE, 1, mapping) == 0);
    assert(uc_core_enable(1, 1000, &next) == 0);
    assert(uc_core_submit(next, 1, 16384, zero, tz, 1001) == 0 && last_buttons == 128);
    assert(uc_core_submit(next, 2, 8192, zero, tz, 1002) == 0 && last_buttons == 32);
    for (unsigned i = 3; i < 150; ++i)
        assert(uc_core_submit(next, i, 0, zero, tz, 1000 + i) == 0);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_TRACE) > 0);
    assert(strstr(logs, "input_accepted") && strstr(logs, "\"dropped\":0}") == NULL);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_INFO) > 0 &&
           strstr(logs, "input_accepted") == NULL);
    assert(uc_core_logs(logs, 8, UC_LOG_TRACE) == 0 && logs[0] == 0);
    assert(uc_core_logs(logs, sizeof logs, UC_LOG_OFF) > 0 && strstr(logs, "\"entries\":[]"));
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
    assert(uc_core_settings(UC_LOG_INFO, 0, mapping) == 2);
    assert(uc_core_enable(2, 1201, &next) == 2);
    delete_failure = 1;
    assert(uc_core_suspend() < 0);
    delete_failure = 0;
    assert(uc_core_tick(1202) == 0);
    create_pending = 0;
    assert(uc_core_enable(1, 1203, &next) == 0);
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
    puts("core: cleanup, stale, timeout, mapping, lifecycle, pending, logs PASS");
    return 0;
}
