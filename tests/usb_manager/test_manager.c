#include "../../src/drivers/usb.h"
#include "../../src/drivers/usb/protocol.h"
#include "../../src/core/uc_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int scans, reads, health, closes, available = 1, attached = 1, gate = 1, report, read_error,
                                         protocol_error;
static int init_ready = 1, enables, pending, disable_error, disables, submits, refreshes,
           lease_error;
static uint64_t active, next_session = 1, sequence_seen;
static uint16_t fixture_vid = 0x045e, fixture_pid = 0x028e;
static int activation_seen;
static const uc_usb_ops ops;
static int scan(void *ctx, uc_usb_device *out)
{
    assert(ctx == &scans);
    scans++;
    if (!available)
        return 0;
    *out = (uc_usb_device){.kind = UC_USB_XINPUT, .vid = fixture_vid, .pid = fixture_pid};
    snprintf(out->node, sizeof(out->node), "ugen0.2\"bad");
    return 1;
}
static int read_packet(void *ctx, uint8_t *out, size_t cap)
{
    assert(ctx == &scans && cap >= 1);
    reads++;
    if (read_error)
        return -1;
    if (!report)
        return 0;
    report--;
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
    assert(ctx == &scans);
    health++;
    return attached;
}
static void close_device(void *ctx)
{
    assert(ctx == &scans);
    closes++;
}
static const uc_usb_ops ops = {scan, read_packet, write_packet, alive, close_device};
int profile_usb_xinput_activation(uint16_t vid, uint16_t pid)
{
    activation_seen = (vid == 0x045e && pid == 0x028e) ||
                      (vid == 0x3537 && (pid == 0x1040 || pid == 0x100f || pid == 0x1098));
    return activation_seen;
}
int profile_usb_input_watchdog(uint16_t vid, uint16_t pid)
{
    return vid == 0x3537 && (pid == 0x1040 || pid == 0x100f || pid == 0x1098);
}
const char *pad_display_name_for_transport(pad_transport transport, uint16_t vid, uint16_t pid,
                                           const char *profile)
{
    assert(transport == PAD_TRANSPORT_USB);
    if (vid == 0x1234 && pid == 0x5678)
        return "Custom \"pad\" \\ model";
    if (vid == 0x3537 && pid == 0x1040)
        return "GameSir Nova Lite";
    if (vid == 0x3537 && pid == 0x100f)
        return "GameSir Nova 2 Lite";
    if (vid == 0x3537 && pid == 0x1098)
        return "GameSir Nova 2 Lite Receiver";
    return profile;
}
int uc_usb_transport_open(uc_usb_transport *out)
{
    (void)out;
    return 0;
}
int uc_backend_can_start(int32_t user)
{
    return user == 1 && gate;
}
void uc_usb_protocol_begin(uc_usb_protocol *p, uc_usb_kind kind, uint64_t now)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->started = now;
    p->ready = init_ready;
}
int uc_usb_protocol_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    (void)p;
    (void)t;
    (void)now;
    return protocol_error ? -1 : 0;
}
int uc_usb_protocol_feed(uc_usb_protocol *p, const uint8_t *b, size_t n, pad_state *out,
                         uint64_t now)
{
    (void)p;
    (void)b;
    (void)n;
    (void)now;
    pad_state_reset(out);
    out->buttons = 0x4000;
    out->lx = 255;
    out->l2 = 255;
    return 1;
}
uint64_t uc_core_active_session(void)
{
    return active;
}
int uc_core_enable(int32_t user, uint64_t now, uint64_t *out)
{
    (void)now;
    assert(user == 1);
    enables++;
    if (pending)
        return 2;
    *out = active = next_session++;
    sequence_seen = 0;
    return 0;
}
int uc_core_disable(void)
{
    disables++;
    if (disable_error)
        return -1;
    active = 0;
    return 0;
}
int uc_core_submit(uint64_t session, uint64_t sequence, uint32_t buttons, const int16_t axes[4],
                   const uint16_t triggers[2], uint64_t now)
{
    (void)now;
    assert(session == active && sequence > sequence_seen && buttons == 0x4000 && axes[0] == 32767 &&
           triggers[0] == 65535);
    sequence_seen = sequence;
    submits++;
    return 0;
}
int uc_core_refresh_source(uint64_t session, uint64_t now)
{
    (void)now;
    assert(session == active);
    refreshes++;
    return lease_error ? -1 : 0;
}
static void install(void)
{
    uc_usb_transport t = {&ops, &scans};
    assert(uc_usb_set_transport(&t) == 0);
}
static void finish(void)
{
    disable_error = 0;
    assert(uc_usb_stop() == 0);
    assert(!uc_usb_running());
    available = attached = gate = init_ready = 1;
    report = read_error = protocol_error = pending = lease_error = 0;
}
int main(void)
{
    uc_usb_set_log_level(0);
    install();
    gate = 0;
    assert(uc_usb_start(1) == -1);
    gate = 1;
    active = 999;
    assert(uc_usb_start(1) == 2);
    active = 0;
    available = 0;
    assert(uc_usb_start(1) == 0 && uc_usb_running());
    uc_usb_poll(0);
    int count = scans;
    uc_usb_poll(100);
    assert(scans == count);
    uc_usb_poll(1000);
    assert(scans == count + 1);
    int idle_closed = closes;
    finish();
    assert(closes == idle_closed + 1); /* Enumeration resources close even without a device. */
    assert(uc_usb_start(1) == 0);
    uc_usb_poll(2000);
    assert(!active && enables == 0 && refreshes == 0);
    report = 1;
    uc_usb_poll(2010);
    assert(active && submits == 1 && refreshes == 1);
    int submitted = submits;
    for (uint64_t now = 2110; now <= 4010; now += 100)
        uc_usb_poll(now);
    assert(submits == submitted &&
           refreshes > 10); /* Long hold renews lease without fake reports/frames. */
    char status[1024];
    assert(uc_usb_status_json(status, sizeof(status)) && strstr(status, "\"input_frames\":1") &&
           strstr(status, "\"vid\":\"045E\"") && strstr(status, "ugen0.2_bad"));
    char small[2];
    assert(!uc_usb_status_json(small, sizeof(small)) && !small[0]);
    int checked = health;
    uc_usb_poll(4050);
    assert(health == checked);
    uc_usb_poll(4110);
    assert(health == checked + 1);
    attached = 0;
    disable_error = 1;
    int closed = closes;
    uc_usb_poll(4210);
    assert(closes == closed + 1 && active && uc_usb_running());
    int scanned = scans;
    uc_usb_poll(4310);
    assert(scans == scanned);
    disable_error = 0;
    uc_usb_poll(4410);
    assert(!active && uc_usb_running());
    attached = 1;
    uc_usb_poll(5210);
    assert(!active);
    report = 1;
    uc_usb_poll(5220);
    assert(active);
    finish();
    assert(uc_usb_start(1) == 0);
    protocol_error = 1;
    uc_usb_poll(6000);
    assert(!uc_usb_running());
    protocol_error = 0;
    count = scans;
    uc_usb_poll(8000);
    assert(scans == count);
    finish();
    assert(uc_usb_start(1) == 0);
    init_ready = 0;
    report = 1;
    uc_usb_poll(9000);
    assert(!active);
    finish();
    assert(uc_usb_start(1) == 0);
    pending = 1;
    report = 1;
    uc_usb_poll(10000);
    assert(!active);
    int called = enables;
    submitted = submits;
    int renewed = refreshes;
    pending = 0;
    uc_usb_poll(10501);
    assert(enables == called + 1 && active && uc_usb_running() && submits == submitted &&
           refreshes == renewed);
    uc_usb_poll(10600);
    assert(submits == submitted && refreshes == renewed);
    report = 1;
    uc_usb_poll(10610);
    assert(submits == submitted + 1 && refreshes == renewed + 1);
    finish();
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(11000);
    assert(active);
    submitted = submits;
    uc_usb_poll(11600);
    assert(!active && !uc_usb_running() && submits == submitted);
    finish();
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(12000);
    assert(active);
    uc_usb_suspend();
    count = scans;
    uc_usb_poll(13000);
    assert(!active && !uc_usb_running() && scans == count);
    finish();
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(14000);
    assert(active);
    lease_error = 1;
    uc_usb_poll(14100);
    assert(!active && !uc_usb_running());
    finish();
    assert(uc_usb_start(1) == 0);
    report = 20;
    int read_before = reads;
    uc_usb_poll(15000);
    assert(reads - read_before == 8);
    finish();
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(16000);
    gate = 0;
    closed = closes;
    uc_usb_poll(16100);
    assert(closes == closed + 1 && !active && !uc_usb_running());
    finish();
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(17000);
    assert(active);
    read_error = 1;
    disable_error = 1;
    closed = closes;
    uc_usb_poll(17100);
    assert(closes == closed + 1 && active);
    disable_error = 0;
    uc_usb_poll(17200);
    assert(!active);
    finish();
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(18000);
    assert(active);
    active = 999;
    int disabled = disables;
    uc_usb_poll(18100);
    assert(active == 999 && disables == disabled && !uc_usb_running());
    active = 0;
    finish();
    static const uint16_t nova_pids[] = {0x1040, 0x100f, 0x1098};
    static const char *nova_models[] = {"GameSir Nova Lite", "GameSir Nova 2 Lite",
                                        "GameSir Nova 2 Lite Receiver"};
    for (unsigned i = 0; i < 3; i++) {
        fixture_vid = 0x3537;
        fixture_pid = nova_pids[i];
        activation_seen = 0;
        assert(uc_usb_start(1) == 0);
        report = 1;
        int renewals = refreshes;
        uc_usb_poll(20000 + i * 1000);
        assert(active && activation_seen && refreshes == renewals);
        uc_usb_poll(20100 + i * 1000);
        assert(refreshes == renewals); /* Dongle presence is not controller liveness. */
        char model_json[2048];
        assert(uc_usb_status_json(model_json, sizeof(model_json)));
        assert(strstr(model_json, nova_models[i]) && strstr(model_json, "xinput-usb"));
        finish();
    }
    fixture_vid = 0x1234;
    fixture_pid = 0x5678;
    assert(uc_usb_start(1) == 0);
    report = 1;
    uc_usb_poll(24000);
    char quoted_json[2048];
    assert(uc_usb_status_json(quoted_json, sizeof(quoted_json)));
    assert(strstr(quoted_json, "Custom \\\"pad\\\" \\\\ model"));
    finish();
    assert(uc_usb_set_transport(NULL) == 0);
    assert(uc_usb_start(1) == -1);
    puts("USB manager checks passed");
    return 0;
}
