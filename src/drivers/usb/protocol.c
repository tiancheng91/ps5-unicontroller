#include "protocol.h"
#include "../common/log.h"
#include <string.h>

#define INIT_DEADLINE_MS 4800u
#define ACK_TIMEOUT_MS 250u
static int fail(uc_usb_protocol *p)
{
    uc_log_error("usb protocol: initialization/input failed kind=%d step=%d attempts=%d", p->kind,
                 p->step, p->attempts);
    p->failed = 1;
    p->ready = 0;
    return -1;
}
void uc_usb_protocol_begin(uc_usb_protocol *p, uc_usb_kind kind, uint64_t now)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->started = now;
    if (kind != UC_USB_SWITCH && kind != UC_USB_XINPUT)
        p->failed = 1;
}
static void advance(uc_usb_protocol *p)
{
    log_line("usb switch: initialization step=%d acknowledged", p->step);
    p->step++;
    p->attempts = p->waiting = p->pending_length = 0;
    if (p->step == 9)
        p->ready = 1;
}
int uc_usb_protocol_rumble(uc_usb_protocol *p, uint8_t strong, uint8_t weak, uint64_t now)
{
    if (!p || !p->ready || p->failed || p->rumble_failed ||
        (p->kind != UC_USB_SWITCH && p->kind != UC_USB_XINPUT))
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
        if (p->kind == UC_USB_SWITCH) {
            const pad_profile *profile = profile_builtin_named("switch");
            pad_output output = {.strong = p->rumble_strong, .weak = p->rumble_weak};
            if (!profile || !profile->build_output ||
                profile->build_output(p->context.bytes, &output, p->rumble_packet,
                                      sizeof(p->rumble_packet)) != 10)
                return rumble_fail(p);
            p->rumble_length = 64; /* USB uses the same motor encoding, zero-padded. */
        } else {
            const uint8_t packet[8] = {0, 8, 0, p->rumble_strong, p->rumble_weak, 0, 0, 0};
            memcpy(p->rumble_packet, packet, sizeof(packet));
            p->rumble_length = sizeof(packet);
        }
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
int uc_usb_protocol_poll(uc_usb_protocol *p, const uc_usb_transport *t, uint64_t now)
{
    static const uint8_t commands[] = {1, 2, 3, 2, 4};
    if (!p || p->failed)
        return -1;
    if (p->kind == UC_USB_XINPUT) {
        if (!p->ready && (now < p->started || now - p->started >= INIT_DEADLINE_MS))
            return fail(p);
        if (p->led_sent)
            return p->ready ? rumble_poll(p, t, now) : 0;
        if (now < p->started || now - p->started >= INIT_DEADLINE_MS)
            return fail(p);
        /* Unknown VID/PID candidates receive no commands until a real 00/14
         * frame proves the interface protocol. Known native XInput identities
         * may need LED activation before it will start streaming. */
        if (!p->xinput_known && !p->ready)
            return 0;
        if (!t || !t->ops || !t->ops->write)
            return fail(p);
        static const uint8_t led[] = {0x01, 0x03, 0x06};
        int written = t->ops->write(t->context, led, sizeof(led));
        if (written < 0 || (written > 0 && written != (int)sizeof(led)))
            return fail(p);
        if (written == (int)sizeof(led))
            p->led_sent = 1;
        return 0;
    }
    if (p->ready)
        return rumble_poll(p, t, now);
    if (now < p->started || now - p->started >= INIT_DEADLINE_MS)
        return fail(p);
    if (!t || !t->ops || !t->ops->write)
        return fail(p);
    if (p->waiting) {
        if (now < p->sent_at)
            return fail(p);
        if (now - p->sent_at < ACK_TIMEOUT_MS)
            return 0;
        p->waiting = 0;
        if (p->attempts >= 3)
            return fail(p);
    }
    if (!p->pending_length) {
        memset(p->pending, 0, sizeof(p->pending));
        if (p->step < 5) {
            p->pending[0] = 0x80;
            p->pending[1] = commands[p->step];
            p->pending_length = 2;
            p->expected_ack = commands[p->step];
        } else {
            const pad_profile *profile = profile_find(0x057e, 0x2009);
            pad_output output = {0};
            output.player = 1;
            if (!profile || !profile->init_report)
                return fail(p);
            int n = profile->init_report(p->context.bytes, p->step - 5, &output, p->pending,
                                         sizeof(p->pending));
            if (n < 11 || n > (int)sizeof(p->pending))
                return fail(p);
            p->pending_length = (unsigned)n;
            p->expected_ack = p->pending[10];
        }
    }
    /* Switch USB interrupt reports are zero-padded to the 64-byte endpoint size.
     * A would-block return retains the exact command/counter, with no busy loop. */
    int written = t->ops->write(t->context, p->pending, sizeof(p->pending));
    if (written < 0 || (written > 0 && written != (int)sizeof(p->pending)))
        return fail(p);
    if (!written)
        return 0;
    ++p->attempts;
    log_line("usb switch: initialization step=%d attempt=%d command=%#x", p->step, p->attempts,
             p->expected_ack);
    p->sent_at = now;
    if (p->step == 4)
        advance(p); /* 80/04 disables USB timeout; no reply is defined. */
    else
        p->waiting = 1;
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
    if (n != 20 || r[0] != 0 || r[1] != 20)
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
    if (!p->ready && (now < p->started || now - p->started >= INIT_DEADLINE_MS))
        return fail(p);
    if (p->kind == UC_USB_XINPUT) {
        int valid = xinput(r, n, state);
        if (valid) {
            p->ready = 1;
            p->rejected_reports = 0;
        } else if (++p->rejected_reports >= 3) {
            return fail(p);
        }
        return valid;
    }
    if (p->waiting && p->step < 5 && n >= 2 && r[0] == 0x81 && r[1] == p->expected_ack) {
        advance(p);
        return 0;
    }
    if (p->waiting && p->step >= 5 && n >= 15 && r[0] == 0x21 && r[14] == p->expected_ack) {
        if (!(r[13] & 0x80))
            return fail(p);
        advance(p);
        return 0;
    }
    if (!p->ready)
        return 0;
    /* Initialization ACKs may arrive late; they never renew the input lease. */
    if (n >= 2 && r[0] == 0x81)
        return 0;
    if (n >= 15 && r[0] == 0x21)
        return 0;
    if (n < 49 || n > 64 || r[0] != 0x30) {
        if (++p->rejected_reports >= 3)
            return fail(p);
        return 0;
    }
    const pad_profile *profile = profile_find(0x057e, 0x2009);
    if (!profile)
        return fail(p);
    pad_state_reset(state);
    if (profile->parse(p->context.bytes, r, (int)n, state) == PARSE_NONE) {
        if (++p->rejected_reports >= 3)
            return fail(p);
        return 0;
    }
    p->rejected_reports = 0;
    return 1;
}
