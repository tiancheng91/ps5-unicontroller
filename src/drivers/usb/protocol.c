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
    if (kind != UC_USB_XINPUT && kind != UC_USB_GENERIC_HID)
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
int uc_usb_protocol_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    if (!p || p->failed)
        return -1;
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
