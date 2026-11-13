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
    int writes, block, short_write;
    size_t length;
    uint8_t bytes[64];
} mock;
static int write_mock(void *context, const uint8_t *bytes, size_t length)
{
    mock *m = context;
    ++m->writes;
    m->length = length;
    memcpy(m->bytes, bytes, length);
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
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 2) && !p.led_sent && p.ready);
    m.block = 0;
    assert(!uc_usb_protocol_poll(&p, &transport, 3) && p.led_sent && m.bytes[0] == 1 &&
           m.bytes[1] == 3 && m.bytes[2] == 6);
    int writes = m.writes;
    assert(!uc_usb_protocol_poll(&p, &transport, 4) && m.writes == writes);
    report[1] = 19;
    assert(!uc_usb_protocol_feed(&p, report, 20, &state, 5));
    assert(!uc_usb_protocol_feed(&p, report, 20, &state, 6));
    assert(uc_usb_protocol_feed(&p, report, 20, &state, 7) < 0);
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && p.led_sent && !p.ready);
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0); /* LED alone is not input. */
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 4799));
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0);
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
    pad_state state, before;
    memset(&state, 0x5a, sizeof(state));
    before = state;
    assert(uc_usb_generic_hid_parse(report, sizeof(report), &state) == UC_USB_HID_UNIMPLEMENTED);
    assert(!memcmp(&state, &before, sizeof(state)));
}
int main(void)
{
    rumble_output();
    selection();
    diagnostics();
    xbox();
    xinput_led_gate();
    puts("USB descriptor and protocol checks passed");
    return 0;
}
