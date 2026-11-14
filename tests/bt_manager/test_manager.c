#include "../../src/drivers/bt.h"
#include "../../src/drivers/protocols/profiles.h"
#include "../../src/core/uc_core.h"
#include "../../src/core/device_options.h"
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct host {
    int placeholder;
};
static struct host fake;
static host_events callbacks;
static int opens, closes, gate = 1, lost, enable_calls, disable_calls, submissions,
                          pending_attempts, cleanup_fail;
static uint64_t active, next_session = 1, sessions[5];
static int ready[5], unavailable_user, busy_user, error_user;
static unsigned submissions_by_user[5];
static uint64_t feedback_session;
static int feedback_calls, feedback_slot;
static uint8_t feedback_strong, feedback_weak;
static int output_ready, pulse_calls, pulse_slot, pulse_accept = 1;
static int16_t submitted_axes[4];
static uint16_t submitted_triggers[2];
static uint32_t submitted_buttons;
static int event_kind, event_slot;
static unsigned char forgotten[6];
static pad_info info = {
    .vid = 0x054c, .pid = 0x0ce6, .profile = "DualSense\"unsafe\\", .addr = {1, 2, 3, 4, 5, 6}};
static pad_state frame = {.buttons = 0x4000,
                          .lx = 128,
                          .ly = 0,
                          .rx = 255,
                          .ry = 128,
                          .l2 = 255,
                          .r2 = 0,
                          .has_battery = 1,
                          .battery_pct = 80};
int uc_backend_transport_ready(void)
{
    return gate;
}
int uc_backend_can_start(int32_t user)
{
    return gate && user >= 1 && user <= 4 && user != unavailable_user;
}
int uc_core_enable_physical(int32_t user, uint64_t now, uint64_t *out)
{
    (void)now;
    assert(user >= 0 && user <= 4);
    enable_calls++;
    if (user > 0 && user == busy_user) {
        *out = 0;
        return 2;
    }
    if (sessions[user] && !ready[user])
        assert(*out == sessions[user]);
    if (!sessions[user])
        sessions[user] = next_session++;
    *out = sessions[user];
    if (user > 0 && user == error_user)
        return -1;
    if (pending_attempts) {
        pending_attempts--;
        return 2;
    }
    ready[user] = 1;
    active = *out;
    return 0;
}
int uc_core_enable_system_assigned(uint64_t now, uint64_t *out)
{
    return uc_core_enable_physical(0, now, out);
}
int uc_core_disable_session(uint64_t session)
{
    assert(session);
    disable_calls++;
    if (cleanup_fail)
        return -1;
    for (int user = 0; user <= 4; user++)
        if (sessions[user] == session) {
            sessions[user] = 0;
            ready[user] = 0;
            if (active == session)
                active = 0;
            return 0;
        }
    return -2;
}
int uc_core_session_active(uint64_t session)
{
    for (int user = 0; user <= 4; user++)
        if (sessions[user] == session && ready[user])
            return 1;
    return 0;
}
int uc_core_read_rumble(uint64_t session, uint64_t now, uint8_t *strong, uint8_t *weak)
{
    (void)now;
    assert(uc_core_session_active(session));
    if (session != feedback_session)
        return 0;
    *strong = 70;
    *weak = 30;
    return 1;
}
uint64_t uc_core_active_session(void)
{
    return active;
}
int uc_core_submit(uint64_t session, uint64_t seq, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now)
{
    (void)now;
    assert(uc_core_session_active(session) && seq);
    for (int user = 0; user <= 4; user++)
        if (sessions[user] == session)
            ++submissions_by_user[user];
    submissions++;
    submitted_buttons = buttons;
    memcpy(submitted_axes, axes, sizeof(submitted_axes));
    memcpy(submitted_triggers, triggers, sizeof(submitted_triggers));
    return 0;
}
int profile_device_known(pad_transport transport, uint16_t vid, uint16_t pid)
{
    (void)transport;
    (void)vid;
    (void)pid;
    return 1;
}
const char *pad_display_name_for_transport(pad_transport transport, uint16_t vid, uint16_t pid,
                                           const char *profile)
{
    (void)transport;
    (void)vid;
    (void)pid;
    return profile ? profile : "Controller";
}
static host_t *open_host(const char *path, const host_events *events, void *userdata)
{
    assert(!strcmp(path, "/tmp/test-bonds.db"));
    assert(userdata == &fake);
    opens++;
    callbacks = *events;
    lost = 0;
    return &fake;
}
void host_close(host_t *h)
{
    assert(h == &fake);
    closes++;
}
void host_poll(host_t *h, int timeout)
{
    assert(h == &fake && timeout == 0);
    int kind = event_kind;
    event_kind = 0;
    if (kind == 1)
        callbacks.on_connect(callbacks.ud, event_slot, &info);
    if (kind == 2)
        callbacks.on_state(callbacks.ud, event_slot, &frame);
    if (kind == 3)
        callbacks.on_disconnect(callbacks.ud, event_slot);
}
int host_output_ready(const host_t *h, int slot)
{
    assert(h == &fake && slot >= 0 && slot < HOST_MAX_PADS);
    return output_ready;
}
int host_rumble_pulse(host_t *h, int slot, int ms)
{
    assert(h == &fake && output_ready && (ms == 200 || ms == 500));
    pulse_calls++;
    pulse_slot = slot;
    return pulse_accept;
}
int host_rumble_feedback(host_t *h, int slot, uint8_t strong, uint8_t weak)
{
    assert(h == &fake && slot >= 0 && slot < HOST_MAX_PADS);
    ++feedback_calls;
    feedback_slot = slot;
    feedback_strong = strong;
    feedback_weak = weak;
    return 1;
}
int host_rumble_stop(host_t *h, int slot)
{
    return host_rumble_feedback(h, slot, 0, 0);
}
int host_transport_lost(const host_t *h)
{
    assert(h == &fake);
    return lost;
}
int host_scan(host_t *h, int seconds)
{
    assert(h == &fake && (seconds == 30 || seconds == 0));
    return 1;
}
int host_pair_device(host_t *h, uint32_t id)
{
    assert(h == &fake);
    return id == 7;
}
int host_discovered_count(const host_t *h)
{
    assert(h == &fake);
    return 1;
}
int host_discovered_get(const host_t *h, int index, host_discovered_info *out)
{
    assert(h == &fake && index == 0);
    *out = (host_discovered_info){.id = 7, .state = "discovered", .has_rssi = 1, .rssi = -45};
    memcpy(out->addr, info.addr, 6);
    snprintf(out->name, sizeof out->name, "pad\"\nname");
    return 1;
}
int host_forget(host_t *h, const unsigned char addr[6])
{
    assert(h == &fake);
    memcpy(forgotten, addr, 6);
    return 1;
}
uint32_t host_scan_id(const host_t *h)
{
    assert(h == &fake);
    return 1;
}
int host_scan_left(const host_t *h)
{
    assert(h == &fake);
    return 7;
}
int host_paired_count(const host_t *h)
{
    assert(h == &fake);
    return 1;
}
int host_paired_get(const host_t *h, int n, host_paired_info *out)
{
    assert(h == &fake && n == 0);
    memset(out, 0, sizeof(*out));
    memcpy(out->addr, info.addr, 6);
    out->vid = info.vid;
    out->pid = info.pid;
    out->connected = 1;
    return 1;
}
static void event(int kind, int slot, uint64_t now)
{
    event_kind = kind;
    event_slot = slot;
    uc_bt_poll(now);
}
static void install(void)
{
    uc_bt_factory factory = {.open = open_host, .userdata = &fake};
    assert(uc_bt_set_factory(&factory, "/tmp/test-bonds.db") == 0);
}
int main(void)
{
    install();
    gate = 0;
    assert(uc_bt_start(1) == -1 && opens == 0);
    gate = 1;
    active = 99;
    assert(uc_bt_start(1) == 0); /* An unrelated USB output does not block HCI. */
    assert(uc_bt_stop() == 0 && active == 99);
    active = 0;
    assert(uc_bt_start(1) == 0 && uc_bt_running());
    assert(uc_bt_start(2) == 2);
    assert(uc_bt_set_factory(NULL, NULL) == 2);
    event(1, 0, 100);
    assert(enable_calls == 0 && submissions == 0);
    pending_attempts = 1;
    event(2, 0, 110);
    assert(enable_calls == 1 && active == 0 && submissions == 0);
    uc_bt_poll(120);
    assert(active && submissions == 1);
    assert(submitted_buttons == 0x4000 && submitted_axes[0] == 0 && submitted_axes[1] == -32768 &&
           submitted_axes[2] == 32767 && submitted_triggers[0] == 65535);
    uc_bt_poll(130);
    assert(submissions == 1); /* Never fake a physical heartbeat. */
    event(1, 1, 140);
    event(2, 1, 150);
    assert(submissions == 1); /* Second source cannot hijack. */
    char json[4096];
    size_t len = uc_bt_status_json(json, sizeof(json));
    assert(len && strlen(json) == len);
    assert(strstr(json, "\"state\":\"on\"") && strstr(json, "\"vid\":\"054C\"") &&
           strstr(json, "\"battery\":80") && strstr(json, "DualSense_unsafe_"));
    assert(strstr(json, "\"discovered\":[") && strstr(json, "\"scan_seconds\":7") &&
           strstr(json, "pad\\\"\\u000aname"));
    int pulses_before = pulse_calls;
    assert(uc_bt_diagnostics_json(json, sizeof json) > 0);
    assert(strstr(json, "\"descriptor_hex\":\"050f\"") && strstr(json, "\"pid_usage\":true") &&
           strstr(json, "\"type\":\"output\"") && strstr(json, "\"descriptor_available\":false"));
    assert(pulse_calls == pulses_before);
    char small[8];
    assert(uc_bt_diagnostics_json(small, sizeof small) == 0 && !small[0]);
    assert(uc_bt_status_json(small, sizeof(small)) == 0 && !small[0]);
    assert(uc_bt_scan(30) == 0 && uc_bt_scan(121) == -1 && uc_bt_scan(0) == 0);
    assert(uc_bt_pair(7) == 0 && uc_bt_pair(8) == -1);
    assert(uc_bt_forget("06:05:04:03:02:01") == 0 && !memcmp(forgotten, info.addr, 6));
    assert(uc_bt_forget("06:05:04:03:02:GG") == -1);
    event(3, 0, 160);
    assert(!active);
    int enabled = enable_calls;
    uc_bt_poll(170);
    assert(enable_calls == enabled); /* Cached other-source input never auto-publishes. */
    event(2, 1, 180);
    assert(active && enable_calls == enabled + 1);
    cleanup_fail = 1;
    int closed = closes;
    assert(uc_bt_stop() == 2 && closes == closed + 1 && uc_bt_running());
    cleanup_fail = 0;
    uc_bt_poll(190);
    assert(!uc_bt_running() && !active && closes == closed + 1);
    assert(uc_bt_start(1) == 0);
    event(1, 0, 200);
    pending_attempts = 3;
    event(2, 0, 210);
    enabled = enable_calls;
    uc_bt_poll(800);
    assert(enable_calls == enabled + 1 && !active);
    uc_bt_poll(805);
    uc_bt_poll(806);
    assert(active); /* May finish identifying, but stale pressed data is never replayed. */
    int previous_submissions = submissions;
    uc_bt_poll(807);
    assert(submissions == previous_submissions);
    lost = 1;
    uc_bt_poll(810);
    assert(!uc_bt_running());
    int opened = opens;
    uc_bt_poll(820);
    assert(opens == opened);
    assert(uc_bt_start(1) == 0);
    event(1, 0, 900);
    pending_attempts = 0;
    event(2, 0, 910);
    assert(active);
    gate = 0;
    uc_bt_poll(920);
    assert(!uc_bt_running() && !active);
    gate = 1;
    /* Notification is physical-link scoped, independent of input/binding. */
    assert(pulse_calls == 0);
    assert(uc_bt_start(1) == 0);
    output_ready = 1;
    event(1, 0, 1000);
    uc_bt_poll(1999);
    assert(pulse_calls == 0);
    uc_bt_poll(2000);
    assert(pulse_calls == 1 && pulse_slot == 0);
    uc_bt_poll(2100);
    assert(pulse_calls == 1);
    event(3, 0, 2200);
    event(1, 0, 2300);
    uc_bt_poll(3300);
    assert(pulse_calls == 2); /* Reconnect gets exactly one new pulse. */
    event(1, 1, 3400);
    event(3, 1, 3500);
    uc_bt_poll(4400);
    assert(pulse_calls == 2); /* Disconnected slot cancels its timer. */
    output_ready = 0;
    event(1, 1, 4500);
    uc_bt_poll(5500);
    assert(pulse_calls == 2);
    output_ready = 1;
    uc_bt_poll(5600);
    assert(pulse_calls == 3); /* Wait for protocol initialization. */
    output_ready = 0;
    event(1, 2, 5700);
    uc_bt_poll(10700);
    output_ready = 1;
    uc_bt_poll(10701);
    assert(pulse_calls == 3); /* Unsupported output expires. */
    event(1, 2, 10800);
    uc_bt_poll(16801);
    assert(pulse_calls == 3); /* No stale pulse after a long polling gap. */
    event(1, 2, 17000);
    uc_bt_set_rumble_enabled(0);
    uc_bt_poll(18000);
    assert(pulse_calls == 3);
    event(1, 2, 18100);
    uc_bt_set_rumble_enabled(1);
    uc_bt_poll(19100);
    assert(pulse_calls == 3); /* Re-enabling does not replay an old connection. */
    pulse_accept = 0;
    event(1, 2, 19200);
    uc_bt_poll(20200);
    uc_bt_poll(20300);
    assert(pulse_calls == 4); /* A failed send never becomes an unbounded retry. */
    event(1, 2, 20400);
    uc_bt_suspend();
    uc_bt_poll(21400);
    assert(pulse_calls == 4 && !uc_bt_running());
    assert(uc_bt_start(1) == 0);
    event(1, 0, 21500);
    gate = 0;
    uc_bt_poll(22500);
    assert(pulse_calls == 4 && !uc_bt_running());
    gate = 1;
    assert(uc_bt_set_factory(NULL, NULL) == 0);
    assert(uc_bt_start(1) == -1); /* Non-PS5 is explicitly unsupported. */
    uc_bt_set_log_level(0);
    assert(uc_bt_stop() == 0);
    install();
    assert(uc_bt_start(1) == 0);
    event(1, 0, 23000);
    event(2, 0, 23001);
    uint64_t first = sessions[1];
    assert(first && ready[1]);
    event(1, 1, 23002);
    event(2, 1, 23003);
    assert(!sessions[2]);
    assert(uc_bt_assign(1, "00:00:00:00:00:00", 2) < 0);   /* Address guards recycled slots. */
    assert(uc_bt_assign(1, "06:05:04:03:02:01", 1) == -1); /* Duplicate user refused. */
    assert(uc_bt_assign(1, "06:05:04:03:02:01", 2) == 0);
    uc_bt_poll(23004);
    assert(!sessions[2]); /* Ownership change needs a fresh report. */
    event(2, 1, 23005);
    uint64_t second = sessions[2];
    assert(second && second != first && ready[1] && ready[2]);
    assert(uc_bt_status_json(json, sizeof json) > 0 && strstr(json, "\"user_id\":2") &&
           strstr(json, "\"pending\":false"));
    feedback_session = second;
    int feedback_before = feedback_calls;
    uc_bt_poll(23006);
    assert(feedback_calls == feedback_before + 1 && feedback_slot == 1 && feedback_strong == 70 &&
           feedback_weak == 30);
    uc_bt_set_rumble_enabled(0);
    assert(feedback_strong == 0 && feedback_weak == 0);
    feedback_before = feedback_calls;
    uc_bt_poll(23007);
    assert(feedback_calls == feedback_before);
    uc_bt_set_rumble_enabled(1);
    unsigned second_frames = submissions_by_user[2];
    event(3, 0, 23008);
    assert(!ready[1] && ready[2] && sessions[2] == second);
    event(2, 1, 23009);
    assert(submissions_by_user[2] == second_frames + 1);
    event(1, 0, 23010);
    event(2, 0, 23011);
    assert(ready[1] && ready[2]);
    unavailable_user = 1;
    uc_bt_poll(23012);
    assert(uc_bt_running() && !ready[1] && ready[2]);
    unavailable_user = 0;
    cleanup_fail = 1;
    event(3, 1, 23013);
    assert(uc_bt_running());
    cleanup_fail = 0;
    sessions[2] = 0;
    ready[2] = 0;
    active = 0; /* Core tick completed the earlier removal. */
    uc_bt_poll(23014);
    assert(!ready[2]);
    assert(uc_bt_stop() == 0);
    assert(uc_bt_start(1) == 0);
    event(1, 0, 24000);
    event(2, 0, 24001);
    assert(ready[1]);
    cleanup_fail = 1;
    int stops_before = feedback_calls;
    assert(uc_bt_assign(0, "06:05:04:03:02:01", 2) == 2);
    assert(feedback_calls == stops_before + 1 && !feedback_strong && !feedback_weak);
    uc_bt_poll(24002);
    assert(ready[1] && !ready[2]);
    cleanup_fail = 0;
    uc_bt_poll(24003);
    assert(!ready[1] && !ready[2]);
    event(2, 0, 24004);
    assert(ready[2]); /* Queued reassignment is retained across cleanup failure. */
    event(1, 1, 24005);
    assert(uc_bt_assign(1, "06:05:04:03:02:01", 3) == 0);
    busy_user = 3;
    event(2, 1, 24006);
    enabled = enable_calls;
    uc_bt_poll(24600);
    assert(enable_calls == enabled &&
           !sessions[3]); /* No token: stale input cannot start output. */
    busy_user = 0;
    event(2, 1, 24601);
    assert(ready[3] && ready[2]);
    assert(uc_bt_stop() == 0);
    assert(uc_bt_start(1) == 0);
    error_user = 1;
    event(1, 0, 25000);
    event(2, 0, 25001);
    assert(!uc_bt_running() && !sessions[1]); /* Fail closed and release shared HCI. */
    enabled = enable_calls;
    for (int i = 0; i < 100; i++)
        uc_bt_poll(25003 + (uint64_t)i);
    assert(enable_calls == enabled && !sessions[1]);
    error_user = 0;
    assert(uc_bt_start(1) == 0); /* Only explicit restart retries creation. */
    event(1, 0, 25200);
    event(2, 0, 25201);
    assert(ready[1] && enable_calls == enabled + 1);
    assert(uc_bt_stop() == 0 && !sessions[1]);
    error_user = 0;
    assert(uc_bt_start(1) == 0);
    error_user = 1;
    cleanup_fail = 1;
    int closed_before_failure = closes;
    event(1, 0, 25300);
    event(2, 0, 25301);
    assert(closes == closed_before_failure + 1 && uc_bt_running());
    enabled = enable_calls;
    uc_bt_poll(25302);
    assert(enable_calls == enabled); /* Cleanup retries must never create. */
    cleanup_fail = 0;
    uc_bt_poll(25303);
    assert(!uc_bt_running() && !sessions[1]);
    error_user = 0;
    /* Experimental resume injects before login, without choosing an account. */
    unavailable_user = 1;
    assert(uc_bt_start(1) == -1);
    assert(uc_bt_start_system_assigned() == 0);
    enabled = enable_calls;
    event(1, 0, 26000);
    event(2, 0, 26001);
    assert(ready[0] && sessions[0] && enable_calls > enabled);
    int sent = submissions;
    frame.buttons = PAD_PS;
    event(2, 0, 26002);
    assert(submissions > sent && submitted_buttons == PAD_PS);
    uint64_t login_device = sessions[0];
    assert(uc_bt_assign(0, "06:05:04:03:02:01", 2) == -1);
    unavailable_user = 0; /* A login must not re-create or rebind the device. */
    event(2, 0, 26003);
    assert(sessions[0] == login_device && !sessions[1] && !sessions[2]);
    event(1, 1, 26004);
    enabled = enable_calls;
    event(2, 1, 26005);
    assert(enable_calls == enabled); /* Only one experiment source. */
    char status_json[8192];
    assert(uc_bt_status_json(status_json, sizeof status_json));
    assert(strstr(status_json, "\"system_assignment\":true") &&
           strstr(status_json, "\"output_active\":true") && strstr(status_json, "\"bound\":false"));
    uc_bt_suspend();
    assert(!uc_bt_running() && !sessions[0]);
    assert(uc_bt_status_json(status_json, sizeof status_json));
    assert(strstr(status_json, "\"state\":\"off\"") &&
           !strstr(status_json, "suspended_manual_restart_required"));
    assert(uc_bt_start_system_assigned() == 0);
    event(1, 0, 27000);
    event(2, 0, 27001);
    assert(ready[0]);
    gate = 0;
    uc_bt_poll(27002);
    assert(!uc_bt_running() && !sessions[0]);
    assert(uc_bt_start_system_assigned() == -1);
    gate = 1;
    char options_path[128];
    snprintf(options_path, sizeof options_path, "/tmp/uc-bt-options-%ld.json", (long)getpid());
    unlink(options_path);
    assert(!uc_device_options_load(options_path));
    assert(!uc_bt_start(1));
    frame.buttons = PAD_CROSS;
    event(1, 0, 40000);
    event(2, 0, 40010);
    const char *key = "classic:06:05:04:03:02:01:054C:0CE6";
    assert(uc_bt_device_options_match(key, active));
    assert(!uc_bt_device_options_match(key, active + 100));
    assert(!uc_device_options_save(key, (uc_device_options){0, 1}));
    int calls_before = feedback_calls;
    uc_bt_device_options_changed();
    assert(feedback_calls == calls_before + 1 && !feedback_strong && !feedback_weak);
    event(2, 0, 40020);
    assert(submitted_buttons == PAD_CIRCLE);
    assert(uc_bt_test_rumble(key, active, 40030) < 0); /* Per-device rumble is disabled. */
    assert(!uc_device_options_save(key, (uc_device_options){1, 1}));
    uc_bt_device_options_changed();
    output_ready = pulse_accept = 1;
    int before_pulse = pulse_calls;
    assert(uc_bt_test_rumble(key, active + 100, 40030) < 0);
    assert(!uc_bt_test_rumble(key, active, 40030) && pulse_calls == before_pulse + 1);
    assert(uc_bt_test_rumble(key, active, 40031) == 2 && pulse_calls == before_pulse + 1);
    uc_bt_set_rumble_enabled(0);
    assert(uc_bt_test_rumble(key, active, 41031) < 0);
    uc_bt_set_rumble_enabled(1);
    output_ready = 0;
    assert(uc_bt_test_rumble(key, active, 41031) < 0);
    assert(!uc_bt_stop());
    assert(!uc_bt_device_options_match(key, 0));
    assert(!unlink(options_path));
    puts("Bluetooth manager checks passed");
    return 0;
}

int host_hid_descriptor_get(const host_t *h, int slot, host_hid_descriptor *out)
{
    (void)h;
    if (slot != 0)
        return 0;
    *out = (host_hid_descriptor){.data = {0x05, 0x0f}, .length = 2};
    return 1;
}
void generic_output_inventory(const uint8_t *data, int length, hid_output_inventory *out)
{
    assert(data[0] == 0x05 && length == 2);
    *out = (hid_output_inventory){.valid = 1, .pid_usage = 1, .count = 1};
    out->reports[0] =
        (hid_report_inventory){.id = 3, .type = 2, .bits = 16, .page_count = 1, .pages = {15}};
}
