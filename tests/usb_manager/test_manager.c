#include "../../src/drivers/usb.h"
#include "../../src/drivers/usb/protocol.h"
#include "../../src/core/uc_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned id;
    int scans, reads, closes, health, available, alive, reports, read_error, reject;
} transport_mock;
static transport_mock ports[4];
static struct {
    uint64_t id, sequence;
    int user, active, pending, submits, refreshes, disable_error;
} sessions[4];
static uint64_t next_session = 1;
static int create_error;
static int gate = 1, wait_creation, init_error, invalid_report, lease_error, feedback, rumble_calls,
           stops;
static uint8_t desired_strong, desired_weak;
static int find(uint64_t id)
{
    for (int i = 0; i < 4; i++)
        if (id && sessions[i].id == id)
            return i;
    return -1;
}
int uc_backend_can_start(int32_t user)
{
    return gate && user >= 1 && user <= 4;
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
    assert(buttons == PAD_CROSS && axes[0] == 32767 && triggers[0] == 65535);
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
    if (!m->available)
        return 0;
    *out = (uc_usb_device){.kind = UC_USB_XINPUT, .vid = 0x045e, .pid = 0x028e};
    snprintf(out->node, sizeof(out->node), "/dev/ugen1.%u", m->id + 1);
    if (m->reject) {
        out->kind = UC_USB_NONE;
        snprintf(out->reason, sizeof(out->reason), "claim_interface");
        out->error_number = 16;
        return 2;
    }
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
    (void)p;
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
int uc_usb_transport_take_removed(char *node, size_t capacity)
{
    if (!hid_removed)
        return 0;
    snprintf(node, capacity, "/dev/ugen1.1");
    hid_removed = 0;
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
    uc_usb_transport ts[4];
    for (unsigned i = 0; i < count; i++) {
        ports[i].id = i;
        ports[i].available = ports[i].alive = 1;
        ts[i] = (uc_usb_transport){&ops, &ports[i]};
    }
    assert(uc_usb_set_transports(ts, count) == 0);
    gate = 1;
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
int main(void)
{
    uc_usb_set_log_level(0);
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
    assert(!uc_usb_set_transport(NULL));
    assert(uc_usb_start(1) < 0);
    puts("USB multi-source manager checks passed");
    return 0;
}
