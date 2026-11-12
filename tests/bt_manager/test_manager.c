#include "../../src/drivers/bt.h"
#include "../../src/core/uc_core.h"
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
static uint64_t active, next_session = 1;
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
int uc_backend_can_start(int32_t user)
{
    return gate && user == 1;
}
int uc_core_enable(int32_t user, uint64_t now, uint64_t *out)
{
    (void)now;
    assert(user == 1);
    enable_calls++;
    if (pending_attempts) {
        pending_attempts--;
        return 2;
    }
    *out = active = next_session++;
    return 0;
}
int uc_core_disable(void)
{
    disable_calls++;
    if (cleanup_fail)
        return -1;
    active = 0;
    return 0;
}
uint64_t uc_core_active_session(void)
{
    return active;
}
int uc_core_submit(uint64_t session, uint64_t seq, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now)
{
    (void)now;
    assert(session == active && seq);
    submissions++;
    submitted_buttons = buttons;
    memcpy(submitted_axes, axes, sizeof(submitted_axes));
    memcpy(submitted_triggers, triggers, sizeof(submitted_triggers));
    return 0;
}
const char *pad_display_name_for_transport(pad_transport transport, uint16_t vid, uint16_t pid,
                                           const char *profile)
{
    (void)transport;
    (void)vid;
    (void)pid;
    return profile ? profile : "Controller";
}
void log_set_level(int level)
{
    assert(level >= 0 && level <= 5);
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
    assert(h == &fake && output_ready && ms == 200);
    pulse_calls++;
    pulse_slot = slot;
    return pulse_accept;
}
int host_transport_lost(const host_t *h)
{
    assert(h == &fake);
    return lost;
}
int host_pair(host_t *h, int seconds)
{
    assert(h == &fake && seconds == 30);
    return 1;
}
int host_forget(host_t *h, const unsigned char addr[6])
{
    assert(h == &fake);
    memcpy(forgotten, addr, 6);
    return 1;
}
int host_pairing_left(const host_t *h)
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
    assert(uc_bt_start(1) == 2);
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
    char small[8];
    assert(uc_bt_status_json(small, sizeof(small)) == 0 && !small[0]);
    assert(uc_bt_pair(30) == 0 && uc_bt_pair(121) == -1);
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
    puts("Bluetooth manager checks passed");
    return 0;
}
