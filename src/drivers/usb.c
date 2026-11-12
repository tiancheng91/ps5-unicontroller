#include "usb.h"
#include "usb/protocol.h"
#include "../core/uc_core.h"
#include "../ps5/uc_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

static struct {
    uc_usb_transport transport;
    uc_usb_device device;
    uc_usb_protocol protocol;
    pad_state latest;
    int injected, enabled, attached, valid, pending, cleanup, health_valid, transport_live;
    int32_t user;
    uint64_t session, sequence, frames, submitted, latest_at, health_at, last_scan, clock;
    int scanned, clock_valid;
    const char *reason;
    unsigned level;
} usb = {.user = -1, .level = 3};
static void log_code(unsigned severity, const char *code)
{
    if (usb.level >= severity)
        fprintf(stderr, "[usb] %s\n", code);
}
void uc_usb_set_log_level(unsigned level)
{
    if (level <= 5)
        usb.level = level;
}
static int valid_transport(const uc_usb_transport *transport)
{
    return transport && transport->ops && transport->ops->scan && transport->ops->read &&
           transport->ops->write && transport->ops->alive && transport->ops->close;
}
static int own_session(void)
{
    return usb.session && uc_core_active_session() == usb.session;
}
/* Dropping USB handles never waits for virtual output deletion. */
static void close_source(void)
{
    if (usb.transport_live && valid_transport(&usb.transport))
        usb.transport.ops->close(usb.transport.context);
    usb.transport_live = 0;
    usb.attached = 0;
    usb.valid = 0;
    usb.health_valid = 0;
    usb.submitted = 0;
    memset(&usb.protocol, 0, sizeof(usb.protocol));
}
static int cleanup_output(void)
{
    if (usb.pending || own_session()) {
        if (uc_core_disable()) {
            usb.cleanup = 1;
            return 2;
        }
    }
    usb.pending = 0;
    usb.session = 0;
    usb.sequence = 0;
    usb.cleanup = 0;
    usb.submitted = 0;
    return 0;
}
static void source_failed(const char *reason, int allow_rescan, uint64_t now)
{
    usb.reason = reason;
    log_code(1, reason);
    close_source();
    usb.last_scan = now;
    usb.scanned = 1;
    if (!allow_rescan)
        usb.enabled = 0;
    (void)cleanup_output();
}
int uc_usb_set_transport(const uc_usb_transport *transport)
{
    if (uc_usb_running())
        return 2;
    if (transport && !valid_transport(transport))
        return -1;
    usb.transport = transport ? *transport : (uc_usb_transport){0};
    usb.injected = transport != NULL;
    return 0;
}
int uc_usb_start(int32_t user)
{
    if (uc_usb_running() || uc_core_active_session())
        return 2;
    if (user <= 0 || !uc_backend_can_start(user)) {
        usb.reason = "start_not_ready";
        return -1;
    }
    if (!usb.injected) {
        memset(&usb.transport, 0, sizeof(usb.transport));
        if (!uc_usb_transport_open(&usb.transport) || !valid_transport(&usb.transport)) {
            usb.reason = "transport_unsupported_or_open_failed";
            return -1;
        }
    }
    usb.enabled = 1;
    usb.transport_live = 1;
    usb.user = user;
    usb.reason = NULL;
    usb.scanned = 0;
    usb.clock_valid = 0;
    usb.frames = 0;
    usb.submitted = 0;
    memset(&usb.device, 0, sizeof(usb.device));
    log_code(3, "started");
    return 0;
}
int uc_usb_stop(void)
{
    usb.enabled = 0;
    close_source();
    if (cleanup_output())
        return 2;
    usb.user = -1;
    log_code(3, "stopped");
    return 0;
}
void uc_usb_suspend(void)
{
    usb.reason = "suspended_manual_restart_required";
    (void)uc_usb_stop();
}
int uc_usb_running(void)
{
    return usb.enabled || usb.attached || usb.pending || usb.session || usb.cleanup;
}
static int16_t axis(uint8_t value)
{
    return value >= 128 ? (int16_t)(((int)value - 128) * 32767 / 127)
                        : (int16_t)(((int)value - 128) * 256);
}
static int submit_latest(uint64_t now)
{
    if (!usb.valid || usb.submitted == usb.frames || !own_session())
        return 0;
    if (now < usb.latest_at || now - usb.latest_at > 500)
        return -1;
    if (usb.sequence == UINT64_MAX)
        return -1;
    const pad_state *state = &usb.latest;
    int16_t axes[4] = {axis(state->lx), axis(state->ly), axis(state->rx), axis(state->ry)};
    uint16_t triggers[2] = {(uint16_t)(state->l2 * 257u), (uint16_t)(state->r2 * 257u)};
    if (uc_core_submit(usb.session, ++usb.sequence, state->buttons, axes, triggers, now))
        return -1;
    usb.submitted = usb.frames;
    return 0;
}
void uc_usb_poll(uint64_t now)
{
    if (usb.clock_valid && uc_usb_running() &&
        (now < usb.clock || (usb.session && now - usb.clock > 500))) {
        usb.clock = now;
        usb.reason = "poll_gap_manual_restart_required";
        (void)uc_usb_stop();
        return;
    }
    usb.clock = now;
    usb.clock_valid = 1;
    if (usb.cleanup) {
        if (cleanup_output())
            return;
        if (!usb.enabled) {
            usb.user = -1;
            return;
        }
    }
    if (!usb.enabled)
        return;
    if (!uc_backend_can_start(usb.user)) {
        usb.reason = "power_or_user_unavailable";
        (void)uc_usb_stop();
        return;
    }
    if (!usb.attached) {
        if (usb.scanned && now - usb.last_scan < 1000)
            return;
        usb.scanned = 1;
        usb.last_scan = now;
        uc_usb_device device;
        memset(&device, 0, sizeof(device));
        usb.transport_live = 1;
        int found = usb.transport.ops->scan(usb.transport.context, &device);
        if (found < 0) {
            source_failed("scan_failed", 0, now);
            return;
        }
        if (!found)
            return;
        usb.device = device;
        usb.device.node[sizeof(usb.device.node) - 1] = 0;
        for (size_t i = 0; usb.device.node[i]; i++) {
            unsigned char c = (unsigned char)usb.device.node[i];
            if (c < 32 || c >= 127 || c == '"' || c == '\\')
                usb.device.node[i] = '_';
        }
        usb.attached = 1;
        if (device.kind != UC_USB_SWITCH && device.kind != UC_USB_XINPUT) {
            source_failed("unsupported_device", 0, now);
            return;
        }
        usb.valid = 0;
        usb.health_valid = 0;
        usb.frames = 0;
        usb.submitted = 0;
        usb.reason = NULL;
        uc_usb_protocol_begin(&usb.protocol, device.kind, now);
        usb.protocol.xinput_known =
            device.kind == UC_USB_XINPUT && profile_usb_xinput_activation(device.vid, device.pid);
        if (usb.level >= 3)
            fprintf(stderr,
                    "[usb] device_opened node=%s vid=%04X pid=%04X iface=%u in=%02X out=%02X\n",
                    usb.device.node, usb.device.vid, usb.device.pid, usb.device.iface,
                    usb.device.ep_in, usb.device.ep_out);
    }
    if (!usb.health_valid || now - usb.health_at >= 100) {
        int alive = usb.transport.ops->alive(usb.transport.context);
        if (alive <= 0) {
            source_failed(alive ? "transport_health_failed" : "device_disconnected", 1, now);
            return;
        }
        usb.health_at = now;
        usb.health_valid = 1;
    }
    if (uc_usb_protocol_poll(&usb.protocol, &usb.transport, now)) {
        source_failed("initialization_failed", 0, now);
        return;
    }
    for (int reads = 0; reads < 8; reads++) {
        uint8_t packet[512];
        int count = usb.transport.ops->read(usb.transport.context, packet, sizeof(packet));
        if (count < 0 || count > (int)sizeof(packet)) {
            source_failed("transport_read_failed", 1, now);
            return;
        }
        if (!count)
            break;
        pad_state state;
        memset(&state, 0, sizeof(state));
        int valid = uc_usb_protocol_feed(&usb.protocol, packet, (size_t)count, &state, now);
        if (valid < 0) {
            source_failed("protocol_failed", 0, now);
            return;
        }
        if (valid && usb.protocol.ready) {
            if (usb.frames == UINT64_MAX) {
                source_failed("input_counter_exhausted", 0, now);
                return;
            }
            if (!usb.frames)
                log_code(3, "first_valid_input");
            usb.latest = state;
            usb.latest_at = now;
            usb.valid = 1;
            ++usb.frames;
        }
    }
    if (usb.session && !own_session()) {
        source_failed("session_lost_manual_restart_required", 0, now);
        return;
    }
    if (!usb.protocol.ready || !usb.valid)
        return;
    if (!usb.session) {
        if (!usb.pending && (now < usb.latest_at || now - usb.latest_at > 500))
            return;
        if (uc_core_active_session()) {
            source_failed("foreign_output_busy", 0, now);
            return;
        }
        uint64_t session = 0;
        int result = uc_core_enable(usb.user, now, &session);
        if (result == 2) {
            usb.pending = 1;
            return;
        }
        if (result || !session) {
            source_failed("virtual_enable_failed", 0, now);
            return;
        }
        usb.session = session;
        usb.pending = 0;
        usb.sequence = 0;
        usb.submitted = 0;
        if (usb.level >= 3)
            fprintf(stderr, "[usb] virtual_bound session=%llu input_frames=%llu\n",
                    (unsigned long long)usb.session, (unsigned long long)usb.frames);
        if (now < usb.latest_at || now - usb.latest_at > 500) {
            /* Slow identification may complete, but its old held input is consumed
             * without submission. The neutral output waits for a genuinely new report. */
            usb.valid = 0;
            usb.submitted = usb.frames;
            return;
        }
    }
    if (submit_latest(now)) {
        source_failed("input_submit_failed", 0, now);
        return;
    }
    /* Interrupt-only devices report changes, not heartbeats. Only a live,
     * previously submitted physical state may extend its source lease. */
    if (!profile_usb_input_watchdog(usb.device.vid, usb.device.pid) && usb.submitted &&
        usb.submitted == usb.frames && own_session() && usb.health_valid && now >= usb.health_at &&
        now - usb.health_at <= 100) {
        if (uc_core_refresh_source(usb.session, now))
            source_failed("source_lease_expired", 0, now);
    }
}
static int append(char *buffer, size_t cap, size_t *used, const char *format, ...)
{
    if (*used >= cap)
        return -1;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer + *used, cap - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= cap - *used)
        return -1;
    *used += (size_t)count;
    return 0;
}
size_t uc_usb_status_json(char *buffer, size_t capacity)
{
    if (!buffer || !capacity)
        return 0;
    size_t used = 0;
    buffer[0] = 0;
#define ADD(...)                                                                                   \
    do {                                                                                           \
        if (append(buffer, capacity, &used, __VA_ARGS__)) {                                        \
            buffer[0] = 0;                                                                         \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)
    const char *state = usb.enabled ? "on" : usb.reason ? "failed" : "off";
    ADD("{\"state\":\"%s\",\"reason\":\"%s\",\"user_id\":%d,\"pending\":%s,\"bound\":%s,"
        "\"devices\":[",
        state, usb.reason ? usb.reason : "", usb.user, usb.pending ? "true" : "false",
        own_session() ? "true" : "false");
    if (usb.attached) {
        char node[sizeof(usb.device.node)];
        size_t i;
        for (i = 0; usb.device.node[i] && i < sizeof(node) - 1; i++) {
            unsigned char c = (unsigned char)usb.device.node[i];
            node[i] = c >= 32 && c < 127 && c != '"' && c != '\\' ? (char)c : '_';
        }
        node[i] = 0;
        const char *profile = usb.device.kind == UC_USB_SWITCH ? "switch-usb" : "xinput-usb";
        const char *model = pad_display_name_for_transport(PAD_TRANSPORT_USB, usb.device.vid,
                                                           usb.device.pid, profile);
        char escaped_model[129];
        size_t escaped = 0;
        for (size_t at = 0; model[at]; at++) {
            unsigned char ch = (unsigned char)model[at];
            if (ch < 32 || ch >= 127 || escaped + 2 >= sizeof(escaped_model)) {
                buffer[0] = 0;
                return 0;
            }
            if (ch == '"' || ch == '\\')
                escaped_model[escaped++] = '\\';
            escaped_model[escaped++] = (char)ch;
        }
        escaped_model[escaped] = 0;
        ADD("{\"node\":\"%s\",\"vid\":\"%04X\",\"pid\":\"%04X\",\"profile\":\"%s\",\"model\":\"%"
            "s\",\"connected\":"
            "true,\"input_frames\":%llu,\"bound\":%s,\"session_id\":%llu}",
            node, usb.device.vid, usb.device.pid, profile, escaped_model,
            (unsigned long long)usb.frames, own_session() ? "true" : "false",
            (unsigned long long)usb.session);
    }
    ADD("]}");
#undef ADD
    return used;
}
