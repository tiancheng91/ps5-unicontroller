#include "protocol.h"
#include "../common/log.h"
#include <string.h>

#define INIT_DEADLINE_MS 4800u
static int fail(uc_usb_protocol *p)
{
    uc_log_error("usb protocol: kind=%d initialization/input failed rejected=%u", p->kind,
                 p->rejected_reports);
    p->failed = 1;
    p->ready = 0;
    return -1;
}
void uc_usb_protocol_begin(uc_usb_protocol *p, uc_usb_kind kind, uint64_t now)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->started = now;
    if (kind != UC_USB_XINPUT && kind != UC_USB_GENERIC_HID && kind != UC_USB_SWITCH2_PRO &&
        kind != UC_USB_SWITCH_PRO)
        p->failed = 1;
}
int uc_usb_protocol_hid_descriptor(uc_usb_protocol *p, const uc_usb_device *device)
{
    if (!p || !device || p->kind != UC_USB_GENERIC_HID || !device->diagnostic.report_valid ||
        device->diagnostic.report_saved != device->diagnostic.report_declared ||
        !uc_usb_generic_hid_begin(&p->hid, device->diagnostic.report,
                                  device->diagnostic.report_saved, device->vid, device->pid))
        return p ? fail(p) : -1;
    log_line("usb generic HID: descriptor initialized %04x:%04x bytes=%u; input only", device->vid,
             device->pid, device->diagnostic.report_saved);
    return 0;
}
int uc_usb_protocol_rumble(uc_usb_protocol *p, uint8_t strong, uint8_t weak, uint64_t now)
{
    if (!p || !p->ready || p->failed || p->rumble_failed || p->kind != UC_USB_XINPUT)
        return -1;
    int changed = !p->rumble_seen || p->rumble_strong != strong || p->rumble_weak != weak;
    p->rumble_seen = 1;
    p->rumble_sample = strong || weak;
    p->rumble_sample_at = now;
    if (changed) {
        p->rumble_strong = strong;
        p->rumble_weak = weak;
        p->rumble_dirty = 1;
        p->rumble_stops = strong || weak ? 0 : 3;
        p->rumble_length = p->rumble_attempts = 0;
    }
    return 0;
}
static int rumble_fail(uc_usb_protocol *p)
{
    uc_log_warn("usb rumble: output disabled kind=%d; input remains enabled", p->kind);
    p->rumble_failed = 1;
    p->rumble_length = p->rumble_attempts = p->rumble_stops = 0;
    p->rumble_sample = p->rumble_dirty = 0;
    p->rumble_strong = p->rumble_weak = 0;
    return 0;
}
static int rumble_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    if (p->rumble_failed)
        return 0;
    if (p->rumble_sample && (now < p->rumble_sample_at || now - p->rumble_sample_at >= 500)) {
        /* Replace even an unsent active packet before attempting a stale write. */
        p->rumble_strong = p->rumble_weak = 0;
        p->rumble_sample = 0;
        p->rumble_dirty = 1;
        p->rumble_stops = 3;
        p->rumble_length = p->rumble_attempts = 0;
    }
    int active = p->rumble_strong || p->rumble_weak;
    if (!p->rumble_dirty && !p->rumble_stops && !active)
        return 0;
    if (!t || !t->ops || !t->ops->write)
        return rumble_fail(p);
    if (p->rumble_length) {
        if (now < p->rumble_queued_at || now - p->rumble_queued_at >= 300 ||
            p->rumble_attempts >= 16)
            return rumble_fail(p);
        if (now >= p->rumble_retry_at && now - p->rumble_retry_at < 20)
            return 0;
    } else {
        /* A first stop bypasses rate limiting; repeated stops/refresh are bounded. */
        if (p->rumble_sent && !(p->rumble_dirty && !active) && now >= p->rumble_sent_at &&
            now - p->rumble_sent_at < 60)
            return 0;
        memset(p->rumble_packet, 0, sizeof(p->rumble_packet));
        const uint8_t packet[8] = {0, 8, 0, p->rumble_strong, p->rumble_weak, 0, 0, 0};
        memcpy(p->rumble_packet, packet, sizeof(packet));
        p->rumble_length = sizeof(packet);
        p->rumble_queued_at = now;
        p->rumble_attempts = 0;
    }
    ++p->rumble_attempts;
    p->rumble_retry_at = now;
    int written = t->ops->write(t->context, p->rumble_packet, p->rumble_length);
    if (written < 0 || (written > 0 && (unsigned)written != p->rumble_length))
        return rumble_fail(p);
    if (!written)
        return 0;
    p->rumble_length = p->rumble_attempts = 0;
    p->rumble_sent = 1;
    p->rumble_sent_at = now;
    p->rumble_dirty = 0;
    if (!active && p->rumble_stops)
        --p->rumble_stops;
    return 0;
}
static int led_unavailable(uc_usb_protocol *p)
{
    p->led_failed = p->rumble_failed = 1;
    log_line("usb xinput: LED activation unavailable after %u attempts; "
             "waiting for valid input, rumble disabled",
             p->led_attempts);
    return 0;
}
/* Switch 1 USB transport setup follows Linux hid-nintendo's sequence:
 * handshake, optional 3M baud, handshake, disable USB timeout, select 0x30.
 * Linux joycon_init / joycon_send_usb / joycon_send_subcmd:
 * https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c
 * No SPI writes, device reset, IMU or vibration enable commands are sent. */
static void switch1_next(uc_usb_protocol *p, uint64_t now)
{
    ++p->switch1_step;
    p->switch1_writes = p->switch1_write_attempts = 0;
    p->switch1_waiting = 0;
    p->switch1_next_at = now + 60;
}
static int switch1_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    static const uint8_t commands[] = {2, 3, 2, 4};
    if (p->ready)
        return 0;
    if (now < p->started || now - p->started >= INIT_DEADLINE_MS) {
        uc_log_error("[usb-switch1] init timeout step=%u writes=%u waiting_ack=%d elapsed_ms=%llu",
                     p->switch1_step, p->switch1_writes, p->switch1_waiting,
                     (unsigned long long)(now >= p->started ? now - p->started : 0));
        return fail(p);
    }
    if (now < p->switch1_next_at || p->switch1_step >= 5)
        return 0;
    if (p->switch1_writes >= 3 || p->switch1_write_attempts >= 16) {
        if (p->switch1_step == 1) {
            uc_log_warn("[usb-switch1] baud ACK unavailable; retaining default baud rate");
            switch1_next(p, now);
            return 0;
        }
        uc_log_error("[usb-switch1] retry exhausted step=%u writes=%u attempts=%u", p->switch1_step,
                     p->switch1_writes, p->switch1_write_attempts);
        return fail(p);
    }
    uint8_t packet[12] = {0x80, 0};
    size_t length = 2;
    if (p->switch1_step < 4)
        packet[1] = commands[p->switch1_step];
    else {
        static const uint8_t neutral[8] = {0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40};
        packet[0] = 1;
        packet[1] = p->switch1_counter & 15;
        memcpy(packet + 2, neutral, sizeof neutral);
        packet[10] = 3;
        packet[11] = 0x30;
        length = sizeof packet;
    }
    if (!t || !t->ops || !t->ops->write)
        return fail(p);
    ++p->switch1_write_attempts;
    int result = t->ops->write(t->context, packet, length);
    uc_log_debug("[usb-switch1] tx step=%u command=%02x:%02x attempt=%u result=%d length=%zu",
                 p->switch1_step, packet[0], p->switch1_step < 4 ? packet[1] : packet[10],
                 p->switch1_write_attempts, result, length);
    if (result < 0 || (result > 0 && result != (int)length))
        return fail(p);
    p->switch1_next_at = now + (result ? 600 : 20);
    if (!result)
        return 0;
    ++p->switch1_writes;
    p->switch1_waiting = 1;
    if (p->switch1_step == 3) { /* 0x80:04 intentionally has no ACK. */
        p->switch1_timeout_disabled = 1;
        switch1_next(p, now);
    } else if (p->switch1_step == 4)
        ++p->switch1_counter;
    return 0;
}
static int switch1_feed(uc_usb_protocol *p, const uint8_t *r, size_t n, pad_state *s, uint64_t now)
{
    if (!p->ready && (now < p->started || now - p->started >= INIT_DEADLINE_MS))
        return fail(p);
    if (n >= 2 && r[0] == 0x81) {
        static const uint8_t commands[] = {2, 3, 2};
        uc_log_debug("[usb-switch1] USB ACK command=%02x length=%zu step=%u waiting=%d", r[1], n,
                     p->switch1_step, p->switch1_waiting);
        if (p->switch1_step < 3 && p->switch1_waiting && r[1] == commands[p->switch1_step])
            switch1_next(p, now);
        return 0;
    }
    if (n >= 15 && r[0] == 0x21) {
        uc_log_debug("[usb-switch1] subcommand ACK flags=%02x id=%02x length=%zu", r[13], r[14], n);
        if (p->switch1_step == 4 && p->switch1_waiting && r[14] == 3) {
            if (!(r[13] & 0x80)) {
                uc_log_error("[usb-switch1] report-mode command rejected flags=%02x", r[13]);
                return fail(p);
            }
            switch1_next(p, now);
        }
        return 0;
    }
    if (!n || r[0] != 0x30 || n < 49 || n > 64) {
        if (p->rejected_reports < 3) {
            ++p->rejected_reports;
            uc_log_debug("[usb-switch1] ignored report id=%02x length=%zu step=%u", n ? r[0] : 0, n,
                         p->switch1_step);
        }
        return 0;
    }
    if (p->switch1_step < 4 || (p->switch1_step == 4 && !p->switch1_writes))
        return 0; /* Never register an output before the handshake/report-mode command. */
    const pad_profile *profile = profile_builtin_named("switch");
    if (!profile || !profile->parse)
        return fail(p);
    pad_state_reset(s);
    if (profile->parse(NULL, r, (int)n, s) != PARSE_FULL)
        return 0;
    if (!p->ready)
        uc_log_info("[usb-switch1] first full input bytes=%zu; experimental input ready", n);
    s->has_motion = 0; /* IMU was not enabled by this USB initialization. */
    p->ready = 1;
    p->switch1_step = 6;
    p->rejected_reports = 0;
    return 1;
}
/* Switch 2 Pro USB protocol facts: Linux hid-nintendo.c switch2_init_controller,
 * switch2_receive_command and switch2_event; command framing is defined by
 * hid-nintendo.h and hid-nintendo-switch2-usb.c switch2_usb_send_cmd.
 * Request length vs response ACK:
 * https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md
 * https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c
 * https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.h
 * Input-only experiment: factory stick calibration, buttons/analog features,
 * report 0x09 and USB init. No flash writes, pairing, IMU or rumble enable.
 * The BLE transport in https://github.com/trevlars/switch2-controllers-linux
 * uses private GATT characteristics and is deliberately not enabled here. */
#define SWITCH2_DEADLINE_MS 10000u
static const uint8_t switch2_commands[][2] = {{0x10, 1}, {2, 4},    {2, 4},   {0x0c, 2},
                                              {0x0c, 4}, {3, 0x0a}, {3, 0x0d}};
#define SWITCH2_STEPS (sizeof(switch2_commands) / sizeof(switch2_commands[0]))
static uint32_t switch2_address(unsigned step)
{
    return step == 1 ? 0x130a8u : 0x130e8u;
}
static unsigned switch2_packed(const uint8_t *p, unsigned axis)
{
    return axis ? (p[1] >> 4) | (unsigned)p[2] << 4 : p[0] | (unsigned)(p[1] & 15) << 8;
}
static void switch2_calibrate(uc_usb_protocol *p, unsigned stick, const uint8_t *data)
{
    for (unsigned i = 0; i < 2; i++) {
        unsigned center = switch2_packed(data, i);
        unsigned positive = switch2_packed(data + 3, i);
        unsigned negative = switch2_packed(data + 6, i);
        if (!center || center >= 4095 || !positive || positive >= 4095 || !negative ||
            negative >= 4095) {
            uc_log_warn("[usb-switch2] invalid factory calibration stick=%u axis=%u; default range",
                        stick, i);
            continue;
        }
        p->switch2_calibration[stick * 2 + i][0] = (uint16_t)center;
        p->switch2_calibration[stick * 2 + i][1] = (uint16_t)negative;
        p->switch2_calibration[stick * 2 + i][2] = (uint16_t)positive;
    }
}
static uint8_t switch2_axis(const uint16_t *cal, unsigned raw, int invert)
{
    int delta = (int)raw - (cal[0] ? cal[0] : 2048);
    int range = cal[0] ? cal[delta < 0 ? 1 : 2] : 2048;
    int value = delta * 128 / range;
    if (invert)
        value = -value;
    value += 128;
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}
static int switch2_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    if (p->ready)
        return 0;
    if (now < p->started || now - p->started >= SWITCH2_DEADLINE_MS) {
        uc_log_error("[usb-switch2] initialization timeout step=%u waiting=%d", p->switch2_step,
                     p->switch2_waiting);
        return fail(p);
    }
    if (p->switch2_step == SWITCH2_STEPS || now < p->switch2_next_at)
        return 0;
    if (p->switch2_attempts >= 16 || p->switch2_writes >= 3 || !t || !t->ops || !t->ops->write) {
        uc_log_error("[usb-switch2] command retry exhausted step=%u", p->switch2_step);
        return fail(p);
    }
    uint8_t packet[16] = {0};
    unsigned step = p->switch2_step;
    packet[0] = switch2_commands[step][0];
    packet[1] = 0x91; /* OUT | OK, USB transport 0, 8-byte header. */
    packet[3] = switch2_commands[step][1];
    if (step == 1 || step == 2) {
        uint32_t address = switch2_address(step);
        packet[5] = 8;
        packet[8] = 9;
        packet[9] = 0x7e;
        for (unsigned i = 0; i < 4; i++)
            packet[12 + i] = (uint8_t)(address >> (8 * i));
    } else if (step >= 3) {
        packet[5] = step == 6 ? 8 : 4;
        packet[8] = step == 5 ? 9 : step == 6 ? 1 : 3; /* buttons + analog only */
        if (step == 6)
            memset(packet + 10, 0xff, 6);
    }
    ++p->switch2_attempts;
    int result = t->ops->write(t->context, packet, 8 + packet[5]);
    uc_log_debug("[usb-switch2] tx step=%u command=%02x:%02x attempt=%u bytes=%u result=%d", step,
                 packet[0], packet[3], p->switch2_attempts, 8 + packet[5], result);
    if (result < 0 || (result && result != 8 + packet[5]))
        return fail(p);
    if (result) {
        p->switch2_waiting = 1;
        ++p->switch2_writes;
    }
    p->switch2_next_at = now + (result ? 600 : 20);
    return 0;
}
static int switch2_feed(uc_usb_protocol *p, const uint8_t *r, size_t n, pad_state *s, uint64_t now)
{
    if (!p->ready && (now < p->started || now - p->started >= SWITCH2_DEADLINE_MS))
        return fail(p);
    if (n && r[0] == 9) {
        if (n < 15 || n > 64 || p->switch2_step != SWITCH2_STEPS)
            return 0;
        pad_state_reset(s);
        const uint8_t a = r[3], b = r[4], c = r[5];
        static const uint32_t right[8] = {PAD_CROSS, PAD_CIRCLE, PAD_SQUARE,  PAD_TRIANGLE,
                                          PAD_R1,    PAD_R2,     PAD_OPTIONS, PAD_R3};
        static const uint32_t left[8] = {PAD_DOWN, PAD_RIGHT, PAD_LEFT,   PAD_UP,
                                         PAD_L1,   PAD_L2,    PAD_CREATE, PAD_L3};
        for (unsigned i = 0; i < 8; i++) {
            if (a & (1u << i))
                s->buttons |= right[i];
            if (b & (1u << i))
                s->buttons |= left[i];
        }
        if (c & 1)
            s->buttons |= PAD_PS;
        if (c & 2)
            s->buttons |= PAD_TOUCHPAD;
        s->l2 = b & 0x20 ? 255 : 0;
        s->r2 = a & 0x20 ? 255 : 0;
        s->lx = switch2_axis(p->switch2_calibration[0], switch2_packed(r + 6, 0), 0);
        s->ly = switch2_axis(p->switch2_calibration[1], switch2_packed(r + 6, 1), 1);
        s->rx = switch2_axis(p->switch2_calibration[2], switch2_packed(r + 9, 0), 0);
        s->ry = switch2_axis(p->switch2_calibration[3], switch2_packed(r + 9, 1), 1);
        if (!p->ready)
            uc_log_info("[usb-switch2] first Pro report bytes=%zu; experimental input ready", n);
        p->ready = 1;
        return 1;
    }
    if (p->switch2_step >= SWITCH2_STEPS || !p->switch2_waiting || n < 8)
        return 0;
    unsigned step = p->switch2_step;
    if (r[0] != switch2_commands[step][0] || r[3] != switch2_commands[step][1] || r[2] != 0 ||
        (r[1] & 0x90))
        return 0; /* Ignore unrelated replies and echoed OUT commands. */
    /* Header byte 5 is ACK in replies (e.g. USB 0xf8), not payload length.
     * Bound all response fields by the actual transfer length, as Linux does. */
    uc_log_debug("[usb-switch2] rx step=%u command=%02x:%02x flags=%02x bytes=%zu ack=%02x", step,
                 r[0], r[3], r[1], n, r[5]);
    if (!(r[1] & 1) || (r[1] & 4))
        return fail(p);
    if (step == 0) {
        if (n < 20)
            return 0;
        if (r[11] != 2) {
            uc_log_error("[usb-switch2] unexpected controller type=%u; expected Pro=2", r[11]);
            return fail(p);
        }
        uc_log_info("[usb-switch2] firmware=%u.%u.%u type=%u", r[8], r[9], r[10], r[11]);
    } else if (step == 1 || step == 2) {
        if (n < 25 || r[8] != 9)
            return 0;
        uint32_t address =
            (uint32_t)r[12] | (uint32_t)r[13] << 8 | (uint32_t)r[14] << 16 | (uint32_t)r[15] << 24;
        if (address != switch2_address(step))
            return 0;
        switch2_calibrate(p, step - 1, r + 16);
    }
    ++p->switch2_step;
    p->switch2_waiting = 0;
    p->switch2_attempts = p->switch2_writes = 0;
    p->switch2_next_at = now + 20;
    if (p->switch2_step == SWITCH2_STEPS)
        uc_log_info("[usb-switch2] configuration complete; waiting for HID report 0x09");
    return 0;
}
int uc_usb_protocol_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    if (!p || p->failed)
        return -1;
    if (p->kind == UC_USB_SWITCH_PRO)
        return switch1_poll(p, t, now);
    if (p->kind == UC_USB_SWITCH2_PRO)
        return switch2_poll(p, t, now);
    if (p->kind == UC_USB_XINPUT && !p->ready &&
        (now < p->started || now - p->started >= INIT_DEADLINE_MS))
        return fail(p);
    if (p->kind == UC_USB_GENERIC_HID)
        return p->hid.initialized ? 0 : fail(p);
    if (p->led_sent || p->led_failed)
        return p->ready ? rumble_poll(p, t, now) : 0;
    /* FF/5D/01 plus validated interrupt endpoints authorizes the standard LED
     * activation, including third-party VID/PIDs that wait for host output. */
    if (!t || !t->ops || !t->ops->write || p->led_attempts >= 16)
        return led_unavailable(p);
    if (p->led_attempts && now >= p->led_last_at && now - p->led_last_at < 20)
        return 0;
    p->led_last_at = now;
    ++p->led_attempts;
    static const uint8_t led[] = {0x01, 0x03, 0x06};
    int written = t->ops->write(t->context, led, sizeof(led));
    if (written < 0 || (written > 0 && written != (int)sizeof(led)))
        return led_unavailable(p);
    if (written == (int)sizeof(led)) {
        p->led_sent = 1;
        log_line("usb xinput: player LED activation sent; waiting for input");
    }
    return 0;
}
static uint8_t axis(const uint8_t *bytes, int invert)
{
    int32_t value = (int16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
    if (invert) {
        value = -value;
        if (value > 32767)
            value = 32767;
    }
    return (uint8_t)(((value + 32768) * 255 + 32767) / 65535);
}
static int xinput(const uint8_t *r, size_t n, pad_state *s)
{
    if (n < 20 || n > 64 || r[0] != 0 || r[1] != 20)
        return 0;
    pad_state_reset(s);
    static const uint32_t first[] = {PAD_UP,      PAD_DOWN,   PAD_LEFT, PAD_RIGHT,
                                     PAD_OPTIONS, PAD_CREATE, PAD_L3,   PAD_R3};
    static const uint32_t second[] = {PAD_L1,    PAD_R1,     PAD_PS,     0,
                                      PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE};
    for (unsigned bit = 0; bit < 8; bit++) {
        if (r[2] & (1u << bit))
            s->buttons |= first[bit];
        if (r[3] & (1u << bit))
            s->buttons |= second[bit];
    }
    s->l2 = r[4];
    s->r2 = r[5];
    if (s->l2)
        s->buttons |= PAD_L2;
    if (s->r2)
        s->buttons |= PAD_R2;
    s->lx = axis(r + 6, 0);
    s->ly = axis(r + 8, 1);
    s->rx = axis(r + 10, 0);
    s->ry = axis(r + 12, 1);
    return 1;
}
int uc_usb_protocol_feed(uc_usb_protocol *p, const uint8_t *r, size_t n, pad_state *state,
                         uint64_t now)
{
    if (!p || p->failed || !r || !state)
        return -1;
    if (p->kind == UC_USB_XINPUT && !p->ready &&
        (now < p->started || now - p->started >= INIT_DEADLINE_MS))
        return fail(p);
    if (p->kind == UC_USB_SWITCH_PRO)
        return switch1_feed(p, r, n, state, now);
    if (p->kind == UC_USB_SWITCH2_PRO)
        return switch2_feed(p, r, n, state, now);
    if (p->kind == UC_USB_GENERIC_HID) {
        int parsed = uc_usb_generic_hid_parse(&p->hid, r, n, state);
        if (parsed < 0) {
            if (++p->rejected_reports >= 3)
                return fail(p);
            return 0;
        }
        if (parsed) {
            if (!p->ready)
                log_line("usb generic HID: first valid input bytes=%zu", n);
            p->ready = 1;
            p->rejected_reports = 0;
        }
        return parsed;
    }
    if (p->kind != UC_USB_XINPUT)
        return fail(p);
    int valid = xinput(r, n, state);
    if (valid) {
        if (!p->ready)
            log_line("usb xinput: first valid input bytes=%zu", n);
        p->ready = 1;
        p->rejected_reports = 0;
    } else if (n >= 3 && r[0] == 1 && r[1] == 3) {
        /* Player LED/status notification is not a malformed input report. */
        return 0;
    } else if (++p->rejected_reports >= 3) {
        return fail(p);
    }
    return valid;
}
