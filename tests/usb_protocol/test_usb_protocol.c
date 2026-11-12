#include "../../src/drivers/usb/protocol.h"
#include <assert.h>
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
    assert(uc_usb_select(0x057e, 0x2009, cfg, sizeof cfg, &device) && device.kind == UC_USB_SWITCH);
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
    assert(uc_usb_protocol_poll(&protocol, NULL, 400) == 0 && !protocol.ready);
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
    memset(report + 6, 0, 8);
    assert(uc_usb_protocol_feed(&protocol, report, 20, &state, 404) == 1);
    assert(state.lx == 128 && state.ly == 128 && state.rx == 128 && state.ry == 128);
    uc_usb_protocol_begin(&protocol, UC_USB_XINPUT, 0);
    assert(uc_usb_protocol_poll(&protocol, NULL, 4800) < 0);
}
typedef struct {
    int writes, block, short_write;
    uint8_t bytes[64];
} mock;
static int write_mock(void *context, const uint8_t *bytes, size_t length)
{
    mock *m = context;
    ++m->writes;
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
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && m.writes == 0);
    assert(uc_usb_protocol_feed(&p, report, sizeof(report), &state, 1) == 1);
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
    p.xinput_known = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 0) && p.led_sent && !p.ready);
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0); /* LED alone is not input. */
    uc_usb_protocol_begin(&p, UC_USB_XINPUT, 0);
    p.xinput_known = 1;
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 4799));
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0);
}

static void switch_protocol(void)
{
    uc_usb_protocol p;
    pad_state state;
    mock m = {0};
    uc_usb_transport transport = {&mock_ops, &m};
    uint8_t reply[64] = {0};
    uint64_t now = 0;
    uc_usb_protocol_begin(&p, UC_USB_SWITCH, now);
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, now));
    assert(!p.waiting && !p.attempts && p.step == 0);
    m.block = 0;
    for (unsigned step = 0; step < 9; step++) {
        int writes = m.writes;
        assert(!uc_usb_protocol_poll(&p, &transport, ++now));
        assert(m.writes == writes + 1);
        if (step == 4) {
            assert(p.step == 5);
            continue;
        }
        memset(reply, 0, sizeof reply);
        if (step < 5) {
            reply[0] = 0x81;
            reply[1] = m.bytes[1];
        } else {
            reply[0] = 0x21;
            reply[13] = 0x80;
            reply[14] = m.bytes[10];
        }
        uint8_t expected = reply[step < 5 ? 1 : 14];
        reply[step < 5 ? 1 : 14] = 0xfe;
        assert(!uc_usb_protocol_feed(&p, reply, sizeof reply, &state, now));
        assert(p.waiting);
        reply[step < 5 ? 1 : 14] = expected;
        assert(!uc_usb_protocol_feed(&p, reply, sizeof reply, &state, now));
        assert(p.step == (int)step + 1);
    }
    assert(p.ready);
    memset(reply, 0, sizeof reply);
    reply[0] = 0x30;
    reply[3] = 4;
    reply[7] = reply[10] = 0x08;
    reply[8] = reply[11] = 0x80;
    assert(uc_usb_protocol_feed(&p, reply, 48, &state, now) == 0);
    assert(uc_usb_protocol_feed(&p, reply, 49, &state, now) == 1 && (state.buttons & PAD_CROSS));
    uc_usb_protocol_begin(&p, UC_USB_SWITCH, 0);
    m.writes = 0;
    assert(!uc_usb_protocol_poll(&p, &transport, 0));
    assert(!uc_usb_protocol_poll(&p, &transport, 249) && m.writes == 1);
    assert(!uc_usb_protocol_poll(&p, &transport, 250) && m.writes == 2);
    assert(!uc_usb_protocol_poll(&p, &transport, 500) && m.writes == 3);
    assert(uc_usb_protocol_poll(&p, &transport, 750) < 0 && m.writes == 3);
    uc_usb_protocol_begin(&p, UC_USB_SWITCH, 0);
    m.block = 1;
    assert(!uc_usb_protocol_poll(&p, &transport, 4799));
    assert(uc_usb_protocol_poll(&p, &transport, 4800) < 0);
    uc_usb_protocol_begin(&p, UC_USB_SWITCH, 0);
    p.step = 5;
    m.block = 0;
    assert(!uc_usb_protocol_poll(&p, &transport, 0));
    memset(reply, 0, sizeof reply);
    reply[0] = 0x21;
    reply[14] = m.bytes[10];
    assert(uc_usb_protocol_feed(&p, reply, 15, &state, 1) < 0); /* matching NACK */
    uc_usb_protocol_begin(&p, UC_USB_SWITCH, 0);
    m.block = 0;
    m.short_write = 1;
    assert(uc_usb_protocol_poll(&p, &transport, 0) < 0);
}
int main(void)
{
    selection();
    xbox();
    switch_protocol();
    xinput_led_gate();
    puts("USB descriptor and protocol checks passed");
    return 0;
}
