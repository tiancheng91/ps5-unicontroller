#include "../../src/drivers/usb/protocol.h"
#include <assert.h>
#include "../../src/drivers/usb/generic_hid.h"
#include <string.h>
#include <stdio.h>
static uint8_t configuration[] = {9, 2, 32, 0, 1,    1, 0,  0x80, 50, 9, 4, 3,    0, 2,  0xff, 0x5d,
                                  1, 0, 7,  5, 0x81, 3, 32, 0,    4,  7, 5, 0x02, 3, 32, 0,    4};
static void selection(void)
{
    uc_usb_device device;
    uint8_t cfg[sizeof(configuration)];
    memcpy(cfg, configuration, sizeof cfg);
    assert(uc_usb_select(0x1234, 0x5678, cfg, sizeof cfg, &device));
    cfg[29] = 8;
    assert(uc_usb_select(0x1234, 0x5678, cfg, sizeof cfg, &device));
    assert(device.packet_out == 8);
    cfg[29] = 4;
    assert(!uc_usb_select(0x1234, 0x5678, cfg, sizeof cfg, &device));
    memcpy(cfg, configuration, sizeof cfg);
    assert(uc_usb_select(0x1234, 0x5678, cfg, sizeof cfg, &device));
    assert(device.kind == UC_USB_XINPUT && device.iface == 3 && device.iface_index == 0 &&
           device.ep_in == 0x81);
    static const uint16_t nova_pids[] = {0x1040, 0x100f, 0x1098};
    for (unsigned i = 0; i < 3; i++)
        assert(uc_usb_select(0x3537, nova_pids[i], cfg, sizeof cfg, &device) &&
               device.kind == UC_USB_XINPUT);
    assert(!uc_usb_select(0x054c, 0x1234, cfg, sizeof cfg, &device));
    assert(!uc_usb_select(0x0e8d, 0x3603, cfg, sizeof cfg, &device));
    cfg[16] = 0x81;
    assert(!uc_usb_select(0x045e, 0x0719, cfg, sizeof cfg, &device));
    cfg[15] = 0x47;
    cfg[16] = 0xd0;
    assert(!uc_usb_select(0x045e, 0x02d1, cfg, sizeof cfg, &device));
    memcpy(cfg, configuration, sizeof cfg);
    cfg[18] = 0;
    assert(!uc_usb_select(1, 2, cfg, sizeof cfg, &device));
    memcpy(cfg, configuration, sizeof cfg);
    cfg[27] = 0x82;
    assert(!uc_usb_select(1, 2, cfg, sizeof cfg, &device));
    memcpy(cfg, configuration, sizeof cfg);
    assert(!uc_usb_select(1, 2, cfg, sizeof(cfg) - 1, &device));
    cfg[14] = 3;
    cfg[15] = 0;
    cfg[16] = 0;
    cfg[22] = 64;
    cfg[29] = 64;
    assert(!uc_usb_select(0x3537, 0x1041, cfg, sizeof cfg, &device)); /* USB DInput deferred. */
    assert(!uc_usb_select(0x3537, 0x1040, cfg, sizeof cfg,
                          &device)); /* ID never overrides protocol. */
    assert(!uc_usb_select(0x057e, 0x2009, cfg, sizeof cfg, &device));
    assert(!uc_usb_select(0x057e, 0x2006, cfg, sizeof cfg, &device));
    cfg[12] = 1;
    assert(!uc_usb_select(0x057e, 0x2009, cfg, sizeof cfg, &device));
    uint8_t ambiguous[55];
    memcpy(ambiguous, configuration, 32);
    memcpy(ambiguous + 32, configuration + 9, 23);
    ambiguous[2] = 55;
    ambiguous[4] = 2;
    ambiguous[34] = 4;
    assert(!uc_usb_select(1, 2, ambiguous, sizeof ambiguous, &device));
}
static void xbox(void)
{
    uc_usb_protocol protocol;
    pad_state state;
    uint8_t report[20] = {0, 20, 0x91, 0x15, 255, 128, 0xff, 0x7f, 0xff, 0x7f, 0, 0x80, 0, 0x80};
    uc_usb_protocol_begin(&protocol, UC_USB_XINPUT, 0);
    assert(!protocol.ready);
    assert(uc_usb_protocol_feed(&protocol, report, 19, &state, 401) == 0);
    report[0] = 1;
    assert(!uc_usb_protocol_feed(&protocol, report, 20, &state, 402));
    report[0] = 0;
    report[1] = 19;
    assert(uc_usb_protocol_feed(&protocol, report, 20, &state, 402) < 0);
    uc_usb_protocol_begin(&protocol, UC_USB_XINPUT, 402);
    report[1] = 20;
    assert(uc_usb_protocol_feed(&protocol, report, 20, &state, 403) == 1 && protocol.ready);
    assert(state.lx == 255 && state.ly == 0 && state.rx == 0 && state.ry == 255);
    assert(state.l2 == 255 && state.r2 == 128);
    assert((state.buttons & (PAD_UP | PAD_OPTIONS | PAD_R3 | PAD_L1 | PAD_PS | PAD_CROSS)) ==
           (PAD_UP | PAD_OPTIONS | PAD_R3 | PAD_L1 | PAD_PS | PAD_CROSS));
    uint8_t padded[32] = {0};
    memcpy(padded, report, sizeof(report));
    assert(uc_usb_protocol_feed(&protocol, padded, sizeof(padded), &state, 404) == 1);
    uint8_t status[] = {1, 3, 6};
    assert(!uc_usb_protocol_feed(&protocol, status, sizeof(status), &state, 404));
    assert(protocol.ready && !protocol.failed);
    memset(report + 6, 0, 8);
    assert(uc_usb_protocol_feed(&protocol, report, 20, &state, 404) == 1);
    assert(state.lx == 128 && state.ly == 128 && state.rx == 128 && state.ry == 128);
    uc_usb_protocol_begin(&protocol, UC_USB_XINPUT, 0);
    assert(uc_usb_protocol_poll(&protocol, NULL, 4800) < 0);
}
typedef struct {
    int writes, block, short_write, error;
    size_t length;
    uint8_t bytes[64];
} mock;
static int write_mock(void *context, const uint8_t *bytes, size_t length)
{
    mock *m = context;
    ++m->writes;
    m->length = length;
    memcpy(m->bytes, bytes, length);
    if (m->error)
        return -1;
    if (m->block)
        return 0;
    return m->short_write ? (int)length - 1 : (int)length;
}
static const uc_usb_ops mock_ops = {.write = write_mock};
static void xinput_led_gate(void)
{
    mock m = {0};
    uc_usb_transport transport = {&mock_ops, &m};
    uc_usb_protocol p;
    pad_state state;
    uint8_t report[20] = {0, 20};
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && m.writes == 1);
    assert(uc_usb_protocol_feed(&p, report, sizeof(report), &state, 1) == 1);
    p.led_sent = 0;
    p.led_attempts = 0;
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 2) && !p.led_sent && p.ready);
    m.block = 0;
    assert(!uc_usb_protocol_poll(&p, &transport, 22) && p.led_sent && m.bytes[0] == 1 &&
           m.bytes[1] == 3 && m.bytes[2] == 6);
    int writes = m.writes;
    assert(!uc_usb_protocol_poll(&p, &transport, 23) && m.writes == writes);
    report[1] = 19;
    assert(!uc_usb_protocol_feed(&p, report, 20, &state, 24));
    assert(!uc_usb_protocol_feed(&p, report, 20, &state, 25));
    assert(uc_usb_protocol_feed(&p, report, 20, &state, 26) < 0);
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && p.led_sent && !p.ready);
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0); /* LED alone is not input. */
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 4799));
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0);
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    m.block = 0;
    m.error = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && p.led_failed && p.rumble_failed &&
           !p.failed);
    report[1] = 20;
    assert(uc_usb_protocol_feed(&p, report, sizeof report, &state, 1) == 1 && p.ready);
    int before = m.writes;
    assert(!uc_usb_protocol_poll(&p, &transport, 1000) && m.writes == before);
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    m.error = 0;
    m.short_write = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && p.led_failed && !p.failed);
    assert(uc_usb_protocol_feed(&p, report, sizeof report, &state, 1) == 1);
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    m.short_write = 0;
    m.block = 1;
    before = m.writes;
    for (unsigned now = 0; now < 1000; ++now)
        assert(!uc_usb_protocol_poll(&p, &transport, now));
    assert(m.writes - before == 16 && p.led_failed && !p.failed);
    assert(uc_usb_protocol_feed(&p, report, sizeof report, &state, 1001) == 1);
}

static void rumble_output(void)
{
    uc_usb_protocol p;
    mock m = {0};
    uc_usb_transport t = {&mock_ops, &m};
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    assert(uc_usb_protocol_rumble(&p, 100, 200, 0) < 0 && !m.writes);
    p.ready = 1;
    assert(!uc_usb_protocol_rumble(&p, 100, 200, 1));
    assert(!uc_usb_protocol_poll(&p, &t, 1) && m.length == 3); /* LED owns first TX. */
    assert(!uc_usb_protocol_poll(&p, &t, 2) && m.length == 8);
    const uint8_t expected[8] = {0, 8, 0, 100, 200, 0, 0, 0};
    assert(!memcmp(m.bytes, expected, sizeof(expected)));
    int written = m.writes;
    assert(!uc_usb_protocol_poll(&p, &t, 3) && m.writes == written);
    assert(!uc_usb_protocol_rumble(&p, 100, 200, 450));
    assert(!uc_usb_protocol_poll(&p, &t, 501) && m.bytes[3] == 100);
    assert(!uc_usb_protocol_poll(&p, &t, 950) && m.bytes[3] == 0 && m.bytes[4] == 0);
    assert(!uc_usb_protocol_poll(&p, &t, 1010));
    assert(!uc_usb_protocol_poll(&p, &t, 1070));
    written = m.writes;
    assert(!uc_usb_protocol_poll(&p, &t, 1130) &&
           m.writes == written); /* Three stops, not forever. */
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    p.ready = p.led_sent = 1;
    m.block = 1;
    assert(!uc_usb_protocol_rumble(&p, 255, 255, 0));
    assert(!uc_usb_protocol_poll(&p, &t, 0));
    assert(!uc_usb_protocol_poll(&p, &t, 300) && p.rumble_failed && p.ready && !p.failed);
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    p.ready = p.led_sent = 1;
    assert(!uc_usb_protocol_rumble(&p, 255, 255, 0));
    assert(!uc_usb_protocol_poll(&p, &t, 0));
    m.block = 0;
    assert(!uc_usb_protocol_poll(&p, &t, 500) && m.bytes[3] == 0 && m.bytes[4] == 0);
}
static void diagnostics(void)
{
    uc_usb_diagnostics d;
    uc_usb_diagnose_config(&d, configuration, sizeof(configuration));
    assert(d.config_valid && d.interface_count == 1 && d.endpoint_count == 2);
    assert(d.interfaces[0].number == 3 && d.interfaces[0].index == 0);
    assert(d.endpoints[0].address == 0x81 && d.endpoints[0].packet_size == 32);
    assert(!strcmp(uc_usb_diagnostic_hint(1, 2, &d), "xinput-candidate"));
    const uint8_t report[] = {5, 1, 9, 5, 0xa1, 1, 0xc0};
    d.interface_count = 0;
    d.report_declared = sizeof(report);
    uc_usb_diagnose_report(&d, report, sizeof(report));
    assert(d.report_valid && d.gamepad &&
           !strcmp(uc_usb_diagnostic_hint(1, 2, &d), "generic-hid-gamepad"));
    assert(!strcmp(uc_usb_diagnostic_hint(0x057e, 0x2009, &d), "switch-pro"));
    uc_usb_diagnose_report(&d, report, sizeof(report) - 1);
    assert(!d.report_valid && !d.gamepad); /* Partial data cannot prove a gamepad. */
    uint8_t keyboard[] = {5, 1, 9, 6, 0xa1, 1, 0xc0};
    uc_usb_diagnose_report(&d, keyboard, sizeof(keyboard));
    assert(d.report_valid && d.keyboard && !d.gamepad);
    const uint8_t malformed[] = {0xc0};
    d.report_declared = 1;
    uc_usb_diagnose_report(&d, malformed, sizeof(malformed));
    assert(!d.report_valid);
    uc_usb_diagnose_config(&d, configuration, sizeof(configuration) - 1);
    assert(!d.config_valid);
}
static void generic_hid(void)
{
    static const uint8_t descriptor[] = {
        5,    1,    9,    5,    0xa1, 1,    0x85, 1,    9,    0x30, 9,    0x31, 0x15,
        0,    0x26, 255,  0,    0x75, 8,    0x95, 2,    0x81, 2,    5,    9,    0x19,
        1,    0x29, 4,    0x15, 0,    0x25, 1,    0x75, 1,    0x95, 4,    0x81, 2,
        0x75, 4,    0x95, 1,    0x81, 3,    0x85, 2,    5,    0x0c, 0x0a, 0x23, 2,
        0x75, 1,    0x95, 1,    0x81, 2,    0x75, 7,    0x81, 3,    0xc0};
    const uint8_t cfg[] = {9, 2, 34, 0,    1, 1,  0,    0x80, 50, 9, 4, 3,    0,
                           1, 3, 0,  0,    0, 9,  0x21, 0x11, 1,  0, 1, 0x22, sizeof(descriptor),
                           0, 7, 5,  0x81, 3, 32, 0,    4};
    uc_usb_device candidate = {.vid = 0x3537, .pid = 0x1041}, selected;
    uc_usb_diagnose_config(&candidate.diagnostic, cfg, sizeof cfg);
    candidate.diagnostic.report_declared = sizeof descriptor;
    candidate.diagnostic.report_interface = 3;
    uc_usb_diagnose_report(&candidate.diagnostic, descriptor, sizeof descriptor);
    assert(uc_usb_select_generic(&candidate, &selected));
    assert(selected.kind == UC_USB_GENERIC_HID && selected.ep_in == 0x81 && !selected.ep_out);
    uc_usb_protocol p;
    uc_usb_protocol_begin(&p, selected.kind, 0);
    assert(uc_usb_protocol_hid_descriptor(&p, &selected) == 0);
    assert(!uc_usb_protocol_poll(&p, NULL, 0)); /* No LED, rumble or SET_REPORT I/O. */
    const uint8_t buttons[] = {1, 255, 128, 2}, home[] = {2, 1}, release_home[] = {2, 0},
                  other[] = {3, 0};
    pad_state state;
    assert(uc_usb_protocol_feed(&p, buttons, sizeof buttons, &state, 10) == 1);
    assert(p.ready && state.lx == 255 && state.ly == 128 && (state.buttons & PAD_CROSS));
    assert(uc_usb_protocol_feed(&p, home, sizeof home, &state, 20) == 1);
    assert((state.buttons & (PAD_PS | PAD_CROSS)) == (PAD_PS | PAD_CROSS) && state.lx == 255);
    assert(uc_usb_protocol_feed(&p, release_home, sizeof release_home, &state, 30) == 1);
    assert(!(state.buttons & PAD_PS) && (state.buttons & PAD_CROSS));
    assert(!uc_usb_protocol_feed(&p, other, sizeof other, &state, 40));
    assert(uc_usb_protocol_rumble(&p, 255, 255, 40) < 0 && !uc_usb_protocol_poll(&p, NULL, 40));
    assert(!uc_usb_protocol_feed(&p, buttons, 2, &state, 41));
    assert(!uc_usb_protocol_feed(&p, buttons, 2, &state, 42));
    assert(uc_usb_protocol_feed(&p, buttons, 2, &state, 43) < 0);
    static const uint8_t plain_descriptor[] = {5, 1,    9,    5,    0xa1, 1,   9, 0x30,
                                               9, 0x31, 0x15, 0,    0x26, 255, 0, 0x75,
                                               8, 0x95, 2,    0x81, 2,    0xc0};
    candidate.diagnostic.report_declared = sizeof plain_descriptor;
    uc_usb_diagnose_report(&candidate.diagnostic, plain_descriptor, sizeof plain_descriptor);
    uc_usb_protocol_begin(&p, UC_USB_GENERIC_HID, 0);
    assert(!uc_usb_protocol_hid_descriptor(&p, &candidate));
    assert(!uc_usb_protocol_poll(&p, NULL, 50000)); /* Change-only HID may be idle for minutes. */
    const uint8_t plain[] = {10, 20};
    assert(uc_usb_protocol_feed(&p, plain, sizeof plain, &state, 50001) == 1);
    assert(state.lx == 10 && state.ly == 20 && state.rx == 128 && !state.buttons);
    candidate.diagnostic.keyboard = 1;
    assert(!uc_usb_select_generic(&candidate, &selected));
    candidate.diagnostic.keyboard = 0;
    candidate.diagnostic.report_declared++;
    assert(!uc_usb_select_generic(&candidate, &selected));
}

int main(void)
{
    generic_hid();
    rumble_output();
    selection();
    diagnostics();
    xbox();
    xinput_led_gate();
    puts("USB descriptor and protocol checks passed");
    return 0;
}
