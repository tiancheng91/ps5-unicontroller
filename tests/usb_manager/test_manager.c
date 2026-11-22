#include "../../src/drivers/usb.h"
#include "../../src/drivers/usb/protocol.h"
#include "../../src/core/uc_core.h"
#include "../../src/core/device_options.h"
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned id;
    int scans, opens, reads, closes, health, available, alive, reports, read_error, reject;
} transport_mock;
static transport_mock ports[4];
static int blocked[32], quarantine_calls;
static struct {
    uint64_t id, sequence;
    int user, active, pending, submits, refreshes, disable_error;
} sessions[4];
static uint64_t next_session = 1;
static int create_error, hid_kind, switch1_kind, switch2_kind, hid_configured, known_device = 1;
static int unavailable_user, switch1_restore_calls, switch1_restore_error;
static int gate = 1, wait_creation, init_error, invalid_report, lease_error, feedback, rumble_calls,
           stops;
static uint8_t desired_strong, desired_weak;
static uint32_t expected_buttons[5] = {PAD_CROSS, PAD_CROSS, PAD_CROSS, PAD_CROSS, PAD_CROSS};
static int find(uint64_t id)
{
    for (int i = 0; i < 4; i++)
        if (id && sessions[i].id == id)
            return i;
    return -1;
}
int uc_backend_can_start(int32_t user)
{
    return gate && user >= 1 && user <= 4 && user != unavailable_user;
}
int uc_backend_transport_ready(void)
{
    return gate;
}
int uc_core_system_assignment_available(uint64_t replacing_session)
{
    for (int i = 0; i < 4; ++i)
        if (sessions[i].id && sessions[i].id != replacing_session &&
            (sessions[i].pending || sessions[i].disable_error))
            return 0;
    return 1;
}
int uc_core_session_active(uint64_t session)
{
    int i = find(session);
    return i >= 0 && sessions[i].active;
}
uint64_t uc_core_active_session(void)
{
    for (int i = 0; i < 4; i++)
        if (sessions[i].active)
            return sessions[i].id;
    return 0;
}
int uc_core_enable_physical(int32_t user, uint64_t now, uint64_t *out)
{
    (void)now;
    int i = find(*out);
    if (i >= 0) {
        assert(sessions[i].user == user && sessions[i].pending);
        if (wait_creation)
            return 2;
        sessions[i].pending = 0;
        sessions[i].active = 1;
        return 0;
    }
    assert(!*out);
    for (int k = 0; k < 4; k++)
        if (sessions[k].id && sessions[k].user == user)
            return 2;
    for (i = 0; i < 4; i++)
        if (!sessions[i].id)
            break;
    if (i == 4)
        return 2;
    sessions[i].id = next_session++;
    sessions[i].user = user;
    *out = sessions[i].id;
    sessions[i].pending = wait_creation;
    sessions[i].active = !wait_creation;
    if (create_error) {
        sessions[i].active = 0;
        sessions[i].disable_error = 1;
        return -1; /* A closing partially created output still belongs to this token. */
    }
    return wait_creation ? 2 : 0;
}
int uc_core_enable_system_assigned(uint64_t now, uint64_t *out)
{
    return uc_core_enable_physical(0, now, out);
}
int uc_core_enable_reselected(uint64_t now, uint64_t *out)
{
    return uc_core_enable_physical(0, now, out);
}
int uc_core_disable_session(uint64_t id)
{
    int i = find(id);
    if (i < 0)
        return -2;
    if (sessions[i].disable_error) {
        sessions[i].active = 0; /* Core closing sessions are not active, but still owned. */
        return -1;
    }
    memset(&sessions[i], 0, sizeof(sessions[i]));
    return 0;
}
int uc_core_submit(uint64_t id, uint64_t sequence, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now)
{
    (void)now;
    int i = find(id);
    assert(i >= 0 && sessions[i].active && sequence > sessions[i].sequence);
    assert(buttons == expected_buttons[sessions[i].user] && axes[0] == 32767 &&
           triggers[0] == 65535);
    sessions[i].sequence = sequence;
    sessions[i].submits++;
    return 0;
}
int uc_core_refresh_source(uint64_t id, uint64_t now)
{
    (void)now;
    int i = find(id);
    assert(i >= 0 && sessions[i].active && sessions[i].submits);
    sessions[i].refreshes++;
    return lease_error ? -1 : 0;
}
int uc_core_read_rumble(uint64_t id, uint64_t now, uint8_t *strong, uint8_t *weak)
{
    (void)now;
    assert(uc_core_session_active(id));
    *strong = desired_strong;
    *weak = desired_weak;
    return feedback;
}
int profile_usb_xinput_activation(uint16_t vid, uint16_t pid)
{
    return vid == 0x045e && pid == 0x028e;
}
int profile_usb_input_watchdog(uint16_t vid, uint16_t pid)
{
    (void)vid;
    (void)pid;
    return 0;
}
int profile_device_known(pad_transport transport, uint16_t vid, uint16_t pid)
{
    (void)transport;
    (void)vid;
    (void)pid;
    return known_device;
}
const char *pad_display_name_for_transport(pad_transport transport, uint16_t vid, uint16_t pid,
                                           const char *profile)
{
    assert(transport == PAD_TRANSPORT_USB);
    (void)vid;
    (void)pid;
    return profile;
}
static int scan(void *ctx, uc_usb_device *out)
{
    transport_mock *m = ctx;
    m->scans++;
    if (!m->available || blocked[m->id])
        return 0;
    *out = (uc_usb_device){.kind = switch1_kind   ? UC_USB_SWITCH_PRO
                                   : switch2_kind ? UC_USB_SWITCH2_PRO
                                   : hid_kind     ? UC_USB_GENERIC_HID
                                                  : UC_USB_XINPUT,
                           .vid = 0x045e,
                           .pid = 0x028e};
    snprintf(out->node, sizeof(out->node), "/dev/ugen1.%u", m->id + 1);
    if (m->reject) {
        out->kind = UC_USB_NONE;
        snprintf(out->reason, sizeof(out->reason), "claim_interface");
        out->error_number = 16;
        return 2;
    }
    ++m->opens;
    return 1;
}
static int read_packet(void *ctx, uint8_t *out, size_t cap)
{
    transport_mock *m = ctx;
    assert(cap);
    m->reads++;
    if (m->read_error)
        return -1;
    if (!m->reports)
        return 0;
    m->reports--;
    out[0] = 1;
    return 1;
}
static int write_packet(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    if (n == 2 && p[0] == 0x80 && p[1] == 5) {
        ++switch1_restore_calls;
        if (switch1_restore_error)
            return -1;
    }
    return (int)n;
}
static int alive(void *ctx)
{
    transport_mock *m = ctx;
    m->health++;
    return m->alive;
}
static void close_device(void *ctx)
{
    ((transport_mock *)ctx)->closes++;
}
static const uc_usb_ops ops = {scan, read_packet, write_packet, alive, close_device};
static int hid_removed;
int uc_usb_transport_quarantine(const uc_usb_transport *t)
{
    transport_mock *m = t->context;
    blocked[m->id] = 1;
    ++quarantine_calls;
    return 1;
}
int uc_usb_transport_take_removed(char *node, size_t capacity)
{
    if (!hid_removed)
        return 0;
    snprintf(node, capacity, "/dev/ugen1.1");
    hid_removed = 0;
    blocked[0] = 0;
    return 1;
}
int uc_usb_transport_open(uc_usb_transport *out)
{
    (void)out;
    return 0;
}
void uc_usb_transport_release(uc_usb_transport *t)
{
    memset(t, 0, sizeof(*t));
}
void uc_usb_protocol_begin(uc_usb_protocol *p, uc_usb_kind kind, uint64_t now)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->started = now;
    p->ready = 1;
    p->switch1_timeout_disabled = kind == UC_USB_SWITCH_PRO; /* Simulate completed init. */
}
int uc_usb_protocol_hid_descriptor(uc_usb_protocol *p, const uc_usb_device *d)
{
    assert(p->kind == UC_USB_GENERIC_HID && d->kind == UC_USB_GENERIC_HID);
    ++hid_configured;
    return 0;
}
int uc_usb_protocol_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    (void)p;
    (void)t;
    (void)now;
    return init_error ? -1 : 0;
}
int uc_usb_protocol_rumble(uc_usb_protocol *p, uint8_t strong, uint8_t weak, uint64_t now)
{
    (void)p;
    (void)now;
    rumble_calls++;
    if (!strong && !weak)
        stops++;
    else
        assert(strong == desired_strong && weak == desired_weak);
    return 0;
}
int uc_usb_protocol_feed(uc_usb_protocol *p, const uint8_t *b, size_t n, pad_state *out,
                         uint64_t now)
{
    (void)p;
    (void)b;
    (void)n;
    (void)now;
    if (invalid_report)
        return 0;
    pad_state_reset(out);
    out->buttons = PAD_CROSS;
    out->lx = 255;
    out->l2 = 255;
    return 1;
}
static void setup(unsigned count)
{
    assert(!uc_usb_running());
    memset(ports, 0, sizeof(ports));
    memset(blocked, 0, sizeof blocked);
    quarantine_calls = 0;
    uc_usb_transport ts[4];
    for (unsigned i = 0; i < count; i++) {
        ports[i].id = i;
        ports[i].available = ports[i].alive = 1;
        ts[i] = (uc_usb_transport){&ops, &ports[i]};
    }
    assert(uc_usb_set_transports(ts, count) == 0);
    gate = 1;
    unavailable_user = 0;
    hid_kind = switch1_kind = switch2_kind = 0;
    switch1_restore_error = 0;
    wait_creation = init_error = invalid_report = lease_error = feedback = 0;
    uc_usb_set_rumble_enabled(1);
}
static void finish(void)
{
    for (int i = 0; i < 4; i++)
        sessions[i].disable_error = 0;
    assert(uc_usb_stop() == 0);
    assert(!uc_usb_running());
}
static int by_user(int user)
{
    for (int i = 0; i < 4; i++)
        if (sessions[i].id && sessions[i].user == user)
            return i;
    return -1;
}
static void test_failed_connection_recycles_slots(void)
{
    char json[32768];
    setup(4);
    /* Four scanners see the same physical node; one init failure is sufficient. */
    for (unsigned i = 0; i < 4; i++)
        ports[i].id = 0;
    assert(!uc_usb_start(1));
    init_error = 1;
    uc_usb_poll(10000);
    assert(quarantine_calls == 1 && ports[0].opens == 1);
    int scans = ports[0].scans;
    for (uint64_t now = 10100; now <= 11000; now += 100)
        uc_usb_poll(now);
    assert(quarantine_calls == 1 && ports[0].opens == 1 && ports[0].scans > scans);
    for (unsigned i = 1; i < 4; i++)
        assert(!ports[i].opens);
    assert(uc_usb_status_json(json, sizeof json));
    assert(strstr(json, "initialization_or_output_failed") && strstr(json, "\"devices\":[]"));
    /* Another device with the same VID/PID still works in the recycled slot. */
    init_error = 0;
    ports[0].id = 1;
    ports[0].reports = 1;
    uc_usb_poll(11100);
    assert(uc_core_active_session() && ports[0].opens == 2);
    finish();

    setup(1);
    assert(!uc_usb_start(1));
    init_error = 1;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        hid_removed = attempt != 0;
        uc_usb_poll(20000 + attempt * 100);
        assert(blocked[0] && ports[0].opens == (int)attempt + 1);
    }
    assert(quarantine_calls == 8); /* More failures than workers cannot exhaust slots. */
    init_error = 0;
    /* Observed removal clears isolation without Stop/Start. */
    hid_removed = 1;
    ports[0].reports = 1;
    uc_usb_poll(20800);
    assert(!blocked[0] && ports[0].opens == 9 && uc_core_active_session());
    finish();

    setup(1);
    assert(!uc_usb_start(1));
    create_error = 1;
    ports[0].reports = 1;
    uc_usb_poll(30000);
    assert(quarantine_calls == 1 && sessions[0].id && sessions[0].disable_error);
    scans = ports[0].scans;
    ports[0].id = 1;
    for (uint64_t now = 30100; now <= 30400; now += 100)
        uc_usb_poll(now);
    assert(ports[0].scans == scans && sessions[0].id); /* Still owns failed output. */
    create_error = sessions[0].disable_error = 0;
    uc_usb_poll(30500);
    assert(!sessions[0].id);
    ports[0].reports = 1;
    uc_usb_poll(30600);
    assert(uc_core_active_session() && ports[0].opens == 2);
    finish();
}

static void test_hid_first_input_deadline(void)
{
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        char json[32768];
        setup(1);
        hid_kind = 1;
        invalid_report = scenario == 1;
        assert(!uc_usb_start(1));
        uc_usb_poll(1000);
        assert(uc_usb_status_json(json, sizeof json));
        assert(strstr(json, "\"waiting_for_input\":true") &&
               strstr(json, "\"first_input_remaining_ms\":30000"));
        for (uint64_t now = 1100; now <= 30900; now += 100) {
            ports[0].reports = scenario == 1; /* Noise does not reset the valid-input deadline. */
            uc_usb_poll(now);
        }
        ports[0].reports = scenario == 2;
        uc_usb_poll(30999);
        assert(!quarantine_calls);
        assert(uc_usb_status_json(json, sizeof json));
        if (scenario == 2) {
            assert(uc_core_active_session() && strstr(json, "\"waiting_for_input\":false"));
        } else {
            assert(!uc_core_active_session() && strstr(json, "\"first_input_remaining_ms\":1"));
        }
        uc_usb_poll(31000);
        if (scenario == 2) {
            for (uint64_t now = 31100; now <= 32000; now += 100)
                uc_usb_poll(now);
            assert(!quarantine_calls && uc_core_active_session());
        } else {
            assert(quarantine_calls == 1 && blocked[0] && !uc_core_active_session());
            assert(uc_usb_status_json(json, sizeof json) &&
                   strstr(json, "hid_first_input_timeout"));
            uc_usb_poll(31100);
            assert(quarantine_calls == 1 && ports[0].opens == 1);
            hid_removed = 1;
            invalid_report = 0;
            ports[0].reports = 1;
            uc_usb_poll(31200);
            assert(ports[0].opens == 2 && uc_core_active_session());
        }
        finish();
    }
}

static void test_account_follows_input_not_scanner(void)
{
    char json[32768];
    setup(3);
    for (unsigned i = 0; i < 3; i++)
        ports[i].available = 0;
    assert(!uc_usb_start(1));
    /* Even the first device may be found by a scanner other than slot zero. */
    unsigned previous = 2;
    ports[previous].available = ports[previous].reports = 1;
    uc_usb_poll(1000);
    assert(by_user(1) >= 0 && sessions[by_user(1)].active);
    uint64_t old_session = sessions[by_user(1)].id;
    for (unsigned attempt = 0; attempt < 9; attempt++) {
        unsigned target = attempt % 3;
        if (target == previous)
            target = (target + 1) % 3;
        ports[previous].alive = ports[previous].available = 0;
        ports[target].alive = ports[target].available = ports[target].reports = 1;
        uint64_t now = 1100 + attempt * 100;
        uc_usb_poll(now);
        uc_usb_poll(now + 1); /* A lower-index scanner may run before old output cleanup. */
        int u = by_user(1);
        assert(u >= 0 && sessions[u].active && sessions[u].id != old_session);
        old_session = sessions[u].id;
        assert(uc_usb_status_json(json, sizeof json) && strstr(json, "\"bound\":true"));
        previous = target;
    }
    finish();

    setup(2);
    ports[1].available = 0;
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(3000);
    int old = by_user(1);
    sessions[old].disable_error = 1;
    ports[0].available = ports[0].alive = 0;
    ports[1].available = ports[1].reports = 1;
    uc_usb_poll(3100);
    assert(sessions[old].id && !sessions[old].active);
    assert(uc_usb_assign(1, "/dev/ugen1.2", 1) < 0); /* Cleanup still reserves the account. */
    for (uint64_t now = 3200; now <= 3700; now += 100)
        uc_usb_poll(now);
    sessions[old].disable_error = 0;
    uc_usb_poll(3800);
    assert(by_user(1) < 0); /* Stale input cannot reserve the now-free account. */
    ports[1].reports = 1;
    uc_usb_poll(3810);
    assert(by_user(1) >= 0 && sessions[by_user(1)].active);
    /* Empty slot zero no longer prevents explicit reassignment on slot one. */
    assert(!uc_usb_assign(1, "/dev/ugen1.2", 1));
    assert(!uc_usb_assign(1, "/dev/ugen1.2", 2));
    ports[1].reports = 1;
    uc_usb_poll(3820);
    assert(by_user(1) < 0 && by_user(2) >= 0);
    ports[1].available = ports[1].alive = 0;
    ports[0].available = ports[0].alive = ports[0].reports = 1;
    uc_usb_poll(3920);
    uc_usb_poll(3921);
    assert(by_user(2) >= 0 && sessions[by_user(2)].active && by_user(1) < 0);
    finish();

    setup(3);
    assert(!uc_usb_start(1));
    ports[0].reports = ports[1].reports = ports[2].reports = 1;
    uc_usb_poll(5000);
    assert(by_user(1) >= 0 && sessions[by_user(1)].submits == 1);
    assert(uc_usb_assign(1, "/dev/ugen1.2", 1) < 0); /* Never steal a live account. */
    assert(!uc_usb_assign(1, "/dev/ugen1.2", 2));
    ports[1].reports = 1;
    uc_usb_poll(5010);
    assert(by_user(1) >= 0 && by_user(2) >= 0 && by_user(3) < 0);
    assert(sessions[by_user(1)].active && sessions[by_user(2)].active);
    finish();

    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(6000);
    unavailable_user = 1;
    uc_usb_poll(6100);
    assert(by_user(1) < 0);
    unavailable_user = 0;
    ports[0].reports = 1;
    uc_usb_poll(6110);
    assert(by_user(1) < 0); /* Logging in again does not bypass explicit reassignment. */
    assert(!uc_usb_assign(0, "/dev/ugen1.1", 1));
    ports[0].reports = 1;
    uc_usb_poll(6120);
    assert(by_user(1) >= 0);
    finish();

    setup(2);
    ports[0].available = ports[1].available = 0;
    assert(!uc_usb_start(1));
    uc_usb_poll(7000);
    unavailable_user = 1;
    uc_usb_poll(7100);
    unavailable_user = 0;
    ports[1].available = ports[1].reports = 1;
    uc_usb_poll(7200);
    assert(by_user(1) < 0); /* Account loss while no device is attached also clears preference. */
    assert(!uc_usb_assign(1, "/dev/ugen1.2", 1));
    ports[1].reports = 1;
    uc_usb_poll(7210);
    assert(by_user(1) >= 0);
    finish();
}

static void test_system_user_reselection(void)
{
    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(0);
    int original = by_user(1);
    assert(original >= 0);
    uint64_t old = sessions[original].id;
    assert(uc_usb_reselect_user(0, "stale-node") < 0);
    uint64_t other = 0;
    assert(!uc_core_enable_physical(2, 1, &other));

    assert(uc_core_session_active(old) && uc_core_session_active(other));
    int closes = ports[0].closes;
    sessions[original].disable_error = 1;
    assert(uc_usb_reselect_user(0, "/dev/ugen1.1") == 2);
    assert(uc_usb_reselect_user(0, "/dev/ugen1.1") == 2);
    uc_usb_poll(1);
    assert(by_user(0) < 0 && ports[0].closes == closes);
    sessions[original].disable_error = 0;
    uc_usb_poll(2);
    assert(by_user(0) < 0 && by_user(1) < 0); /* No stale report replay. */
    ports[0].reports = 1;
    uc_usb_poll(3);
    assert(by_user(0) >= 0 && by_user(1) < 0);
    assert(uc_core_session_active(other) && !uc_usb_reselection_pending());
    assert(!uc_core_disable_session(other));
    assert(sessions[by_user(0)].id != old && sessions[by_user(0)].submits == 1);
    char json[8192];
    assert(uc_usb_status_json(json, sizeof json) && strstr(json, "\"system_assignment\":true") &&
           strstr(json, "\"output_active\":true") &&
           strstr(json, "\"user_id\":0,\"connected\":true"));
    /* The service preference remains 1, never overwritten by a guessed final owner. */
    assert(strstr(json, "\"user_id\":1,\"pending\":"));
    finish();
}
int main(void)
{
    test_system_user_reselection();
    test_account_follows_input_not_scanner();
    test_hid_first_input_deadline();
    test_failed_connection_recycles_slots();
    uc_usb_set_log_level(0);
    setup(1);
    switch1_kind = 1;
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(0);
    assert(by_user(1) >= 0);
    int before_refresh = sessions[by_user(1)].refreshes;
    uc_usb_poll(100);
    assert(sessions[by_user(1)].refreshes == before_refresh);
    char switch1_status[8192];
    assert(uc_usb_status_json(switch1_status, sizeof switch1_status) &&
           strstr(switch1_status, "switch-pro-usb") &&
           strstr(switch1_status, "\"experimental\":true") &&
           strstr(switch1_status, "\"rumble_available\":false"));
    int restore_before = switch1_restore_calls;
    switch1_restore_error = 1; /* Cleanup continues even if timeout restore cannot be sent. */
    finish();
    assert(switch1_restore_calls == restore_before + 1 && ports[0].closes == 1 && by_user(1) < 0);
    assert(!uc_usb_stop() && switch1_restore_calls == restore_before + 1);
    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(0);
    assert(by_user(1) >= 0);
    int closes_before_account_loss = ports[0].closes;
    unavailable_user = 1;
    uc_usb_poll(1);
    assert(by_user(1) < 0 && ports[0].closes == closes_before_account_loss);
    char account_status[8192];
    assert(uc_usb_status_json(account_status, sizeof account_status) &&
           strstr(account_status, "user_unavailable") &&
           strstr(account_status, "\"connected\":true"));
    assert(uc_usb_assign(0, "/dev/ugen1.1", 1) < 0);
    assert(!uc_usb_assign(0, "/dev/ugen1.1", 2));
    uc_usb_poll(2);
    assert(by_user(2) < 0); /* An old held input must not transfer accounts. */
    ports[0].reports = 1;
    uc_usb_poll(3);
    assert(by_user(2) >= 0 && by_user(1) < 0);
    finish();
    setup(1);
    switch2_kind = 1;
    assert(!uc_usb_start(1));
    uc_usb_poll(0);
    assert(by_user(1) < 0);
    ports[0].reports = 1;
    uc_usb_poll(1);
    assert(by_user(1) >= 0);
    int refresh_before = sessions[by_user(1)].refreshes;
    uc_usb_poll(100);
    assert(sessions[by_user(1)].refreshes ==
           refresh_before); /* Presence cannot keep Switch 2 buttons held. */
    char switch2_status[8192];
    assert(uc_usb_status_json(switch2_status, sizeof switch2_status) &&
           strstr(switch2_status, "switch2-pro-usb") &&
           strstr(switch2_status, "\"experimental\":true") &&
           strstr(switch2_status, "\"rumble_available\":false") &&
           strstr(switch2_status, "\"read_attempts\":"));
    ports[0].alive = 0;
    uc_usb_poll(201);
    assert(by_user(1) < 0);
    finish();
    setup(1);
    hid_kind = 1;
    known_device = 0;
    int configurations_before = hid_configured;
    assert(!uc_usb_start(1));
    uc_usb_poll(0);
    assert(by_user(1) < 0); /* Enumeration alone must not create a virtual pad. */
    ports[0].reports = 1;
    uc_usb_poll(1);
    assert(hid_configured == configurations_before + 1 && by_user(1) >= 0);
    char hid_status[8192];
    assert(uc_usb_test_rumble("usb:/dev/ugen1.1:045E:028E", sessions[by_user(1)].id, 2) < 0);
    assert(uc_usb_status_json(hid_status, sizeof hid_status) &&
           strstr(hid_status, "generic-hid-usb") &&
           strstr(hid_status, "\"rumble_available\":false") &&
           strstr(hid_status, "\"rumble\":false") && strstr(hid_status, "\"known_device\":false"));
    ports[0].alive = 0;
    uc_usb_poll(101);
    assert(by_user(1) < 0 || !sessions[by_user(1)].active);
    ports[0].alive = 1;
    ports[0].reports = 1;
    uc_usb_poll(202);
    assert(hid_configured == configurations_before + 2 && sessions[by_user(1)].active);
    finish();
    known_device = 1;
    setup(1);
    ports[0].reject = 1;
    assert(!uc_usb_start(1));
    uc_usb_poll(0);
    char diagnostic[8192];
    assert(uc_usb_status_json(diagnostic, sizeof(diagnostic)));
    assert(strstr(diagnostic, "claim_interface") && strstr(diagnostic, "\"errno\":16"));
    assert(!uc_core_active_session());
    assert(uc_usb_diagnostics_json(diagnostic, sizeof(diagnostic)));
    assert(strstr(diagnostic, "config_descriptor_hex") &&
           strstr(diagnostic, "report_descriptor_hex"));
    char diagnostic_tiny[8];
    assert(!uc_usb_diagnostics_json(diagnostic_tiny, sizeof(diagnostic_tiny)));
    uc_usb_poll(100);
    assert(uc_usb_status_json(diagnostic, sizeof(diagnostic)));
    const char *entry = strstr(diagnostic, "claim_interface");
    assert(entry && !strstr(entry + 1, "claim_interface")); /* Deduplicated scan rejection. */
    ports[0].reject = ports[0].available = 0;
    hid_removed = 1;
    uc_usb_poll(200);
    assert(uc_usb_status_json(diagnostic, sizeof(diagnostic)));
    assert(!strstr(diagnostic, "claim_interface"));
    ports[0].available = 1;
    uc_usb_poll(300);
    assert(uc_usb_status_json(diagnostic, sizeof(diagnostic)));
    assert(!strstr(diagnostic, "claim_interface"));
    finish();
    setup(4);
    assert(uc_usb_start(1) == 0);
    uc_usb_poll(0);
    assert(!uc_core_active_session());
    for (unsigned i = 0; i < 4; i++)
        ports[i].reports = 1;
    uc_usb_poll(10);
    assert(by_user(1) >= 0 && by_user(2) < 0);
    assert(uc_usb_assign(1, "stale-node", 2) < 0);
    assert(!uc_usb_assign(1, "/dev/ugen1.2", 2));
    assert(!uc_usb_assign(2, "/dev/ugen1.3", 3));
    assert(!uc_usb_assign(3, "/dev/ugen1.4", 4));
    uc_usb_poll(20);
    assert(by_user(2) < 0); /* Assignment cannot replay input captured before its user selection. */
    for (unsigned i = 1; i < 4; i++)
        ports[i].reports = 1;
    uc_usb_poll(30);
    for (int user = 1; user <= 4; user++)
        assert(by_user(user) >= 0);
    assert(uc_usb_assign(1, "/dev/ugen1.2", 1) < 0);
    char json[32768];
    assert(uc_usb_status_json(json, sizeof(json)) && strstr(json, "\"slot\":3") &&
           strstr(json, "\"user_id\":4"));
    char tiny[2];
    assert(!uc_usb_status_json(tiny, sizeof(tiny)) && !tiny[0]);
    int u1 = by_user(1), u2 = by_user(2), before = sessions[u1].submits;
    for (uint64_t now = 100; now <= 2000; now += 100)
        uc_usb_poll(now);
    assert(sessions[u1].submits == before && sessions[u1].refreshes > 10);
    feedback = 1;
    desired_strong = 100;
    desired_weak = 200;
    int r = rumble_calls;
    uc_usb_poll(2010);
    assert(rumble_calls >= r + 4);
    int stopped = stops;
    uc_usb_set_rumble_enabled(0);
    assert(stops >= stopped + 4);
    r = rumble_calls;
    uc_usb_poll(2020);
    assert(rumble_calls == r);
    feedback = 0;
    sessions[u1].disable_error = 1;
    ports[0].alive = 0;
    int c = ports[0].closes;
    uc_usb_poll(2100);
    assert(ports[0].closes == c + 1 && sessions[u1].id && sessions[u2].active);
    sessions[u1].disable_error = 0;
    uc_usb_poll(2110);
    assert(by_user(1) < 0 && by_user(2) >= 0);
    finish();
    setup(1);
    ports[0].available = 0;
    assert(!uc_usb_start(1));
    uc_usb_poll(3000);
    int scans = ports[0].scans;
    uc_usb_poll(3099);
    assert(ports[0].scans == scans);
    uc_usb_poll(3100);
    assert(ports[0].scans == scans + 1);
    c = ports[0].closes;
    finish();
    assert(ports[0].closes == c + 1);
    setup(1);
    assert(!uc_usb_start(1));
    wait_creation = 1;
    ports[0].reports = 1;
    uc_usb_poll(4000);
    int pending_user = by_user(1);
    assert(pending_user >= 0 && !sessions[pending_user].active);
    uc_usb_poll(4400);
    wait_creation = 0;
    uc_usb_poll(4600);
    assert(sessions[pending_user].active && !sessions[pending_user].submits &&
           !sessions[pending_user].refreshes);
    ports[0].reports = 1;
    uc_usb_poll(4610);
    assert(sessions[pending_user].submits == 1);
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    invalid_report = 1;
    ports[0].reports = 1;
    uc_usb_poll(5000);
    assert(!uc_core_active_session());
    invalid_report = 0;
    ports[0].reports = 1;
    uc_usb_poll(5010);
    assert(uc_core_active_session());
    uc_usb_suspend();
    scans = ports[0].scans;
    uc_usb_poll(5100);
    assert(!uc_usb_running() && !uc_core_active_session() && ports[0].scans == scans);
    assert(uc_usb_status_json(json, sizeof json));
    assert(strstr(json, "\"state\":\"off\"") && !strstr(json, "suspended_manual_restart_required"));
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(6000);
    assert(uc_core_active_session());
    uc_usb_poll(6600);
    assert(!uc_usb_running() && !uc_core_active_session());
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    init_error = 1;
    uc_usb_poll(7000);
    assert(!uc_core_active_session());
    assert(uc_usb_status_json(json, sizeof(json)) &&
           strstr(json, "initialization_or_output_failed"));
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 20;
    int reads = ports[0].reads;
    uc_usb_poll(8000);
    assert(ports[0].reads - reads == 8);
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(9000);
    u1 = by_user(1);
    sessions[u1].disable_error = 1;
    ports[0].read_error = 1;
    c = ports[0].closes;
    uc_usb_poll(9010);
    assert(ports[0].closes == c + 1 && sessions[u1].id);
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(10000);
    u1 = by_user(1);
    memset(&sessions[u1], 0, sizeof(sessions[u1]));
    uc_usb_poll(10010);
    assert(!uc_core_active_session());
    finish();
    setup(1);
    assert(!uc_usb_start(1));
    create_error = 1;
    ports[0].reports = 1;
    uc_usb_poll(11000);
    u1 = by_user(1);
    assert(u1 >= 0 && !sessions[u1].active && uc_usb_running());
    assert(uc_usb_stop() == 2); /* Failed first creation keeps the owned closing token. */
    sessions[u1].disable_error = 0;
    create_error = 0;
    uc_usb_poll(11010);
    assert(by_user(1) < 0 && !uc_usb_running());
    finish();
    /* Same-model physical devices keep independent layout and motor options. */
    char options_path[128];
    snprintf(options_path, sizeof options_path, "/tmp/uc-usb-options-%ld.json", (long)getpid());
    unlink(options_path);
    assert(!uc_device_options_load(options_path));
    setup(2);
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(20000);
    assert(!uc_usb_assign(1, "/dev/ugen1.2", 2));
    ports[1].reports = 1;
    uc_usb_poll(20010);
    const char *key = "usb:/dev/ugen1.1:045E:028E";
    assert(uc_usb_device_options_match(key, sessions[by_user(1)].id));
    assert(!uc_usb_device_options_match(key, sessions[by_user(2)].id));
    assert(!uc_device_options_save(key, (uc_device_options){0, 1}));
    expected_buttons[1] = PAD_CIRCLE;
    uc_usb_device_options_changed();
    feedback = 1;
    r = rumble_calls;
    before = sessions[by_user(1)].submits;
    uc_usb_poll(20020);
    assert(sessions[by_user(1)].submits == before + 1 && rumble_calls == r + 1);
    assert(uc_usb_status_json(json, sizeof json) && strstr(json, "\"swap_ab\":true"));
    finish();
    assert(!uc_usb_device_options_match(key, 0));
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(20100); /* Reconnect keeps the saved flip. */
    assert(sessions[by_user(1)].submits > 0);
    finish();
    assert(!unlink(options_path));
    assert(!uc_device_options_load(options_path));
    setup(1);
    known_device = 0;
    expected_buttons[1] = PAD_CROSS;
    assert(!uc_usb_start(1));
    ports[0].reports = 1;
    uc_usb_poll(20200);
    feedback = 1;
    r = rumble_calls;
    uc_usb_poll(20210);
    assert(rumble_calls == r); /* Unknown XInput identities are opt-in too. */
    assert(uc_usb_status_json(json, sizeof json) && strstr(json, "\"known_device\":false") &&
           strstr(json, "\"rumble\":false"));
    assert(!uc_device_options_save(key, (uc_device_options){1, 0}));
    uc_usb_device_options_changed();
    uc_usb_poll(20220);
    assert(rumble_calls == r + 1);
    uint64_t test_session = sessions[by_user(1)].id;
    desired_strong = desired_weak = 200;
    assert(uc_usb_test_rumble(key, test_session + 100, 20230) < 0);
    assert(!uc_usb_test_rumble(key, test_session, 20230));
    r = rumble_calls;
    assert(uc_usb_test_rumble(key, test_session, 20231) == 2 && rumble_calls == r);
    uc_usb_poll(20240);
    assert(rumble_calls == r + 1 && uc_core_session_active(test_session));
    int before_stop = stops;
    uc_usb_poll(20730);
    assert(stops == before_stop + 1); /* Exact pulse deadline sends neutral motors. */
    assert(uc_usb_test_rumble(key, test_session, 20731) == 2);
    uc_usb_poll(21000); /* Keep the transport lease alive between test requests. */
    assert(!uc_usb_test_rumble(key, test_session, 21230));
    uc_usb_set_rumble_enabled(0);
    r = rumble_calls;
    uc_usb_poll(21240);
    assert(rumble_calls == r && uc_usb_test_rumble(key, test_session, 22230) < 0);
    assert(uc_core_session_active(test_session));
    finish();
    assert(!unlink(options_path));
    assert(!uc_usb_set_transport(NULL));
    assert(uc_usb_start(1) < 0);
    puts("USB multi-source manager checks passed");
    return 0;
}
