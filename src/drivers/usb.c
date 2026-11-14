#include "../logging.h"
#include "usb.h"
#include "../core/device_options.h"
#include "usb/protocol.h"
#include "../core/uc_core.h"
#include "../ps5/uc_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#define USB_SLOTS 4u
typedef struct {
    uc_usb_transport transport;
    uc_usb_device device;
    uc_usb_protocol protocol;
    pad_state latest;
    int attached, valid, pending, cleanup, health_valid, transport_live, native_lease;
    char options_key[UC_DEVICE_KEY_MAX];
    int known;
    uint64_t rumble_test_until, rumble_test_last;
    int rumble_test_seen;
    uc_device_options options;
    int scanned, assigning, failed;
    int32_t user, next_user;
    uint64_t session, sequence, frames, submitted, latest_at, health_at, last_scan;
    const char *reason;
} usb_slot;
static struct {
    usb_slot slots[USB_SLOTS];
    uc_usb_transport injected[USB_SLOTS];
    unsigned injected_count, count, level;
    uc_usb_device rejected[16];
    unsigned rejected_count;
    int enabled, rumble, clock_valid;
    uint64_t clock, last_presence;
    int presence_valid;
    const char *reason;
} usb = {.level = 3, .rumble = 1};
static int transport_valid(const uc_usb_transport *t)
{
    return t && t->ops && t->ops->scan && t->ops->read && t->ops->write && t->ops->alive &&
           t->ops->close;
}
static int active(const usb_slot *s)
{
    return s->session && uc_core_session_active(s->session);
}
static void log_code(unsigned level, unsigned slot, const char *code)
{
    if (usb.level >= level)
        uc_log_at(level, "[usb] slot=%u %s", slot, code);
}
void uc_usb_set_log_level(unsigned level)
{
    if (level <= 5)
        usb.level = level;
}
static void stop_motors(usb_slot *s, uint64_t now)
{
    s->rumble_test_until = 0;
    if (s->attached && s->protocol.ready && !s->protocol.failed) {
        (void)uc_usb_protocol_rumble(&s->protocol, 0, 0, now);
        (void)uc_usb_protocol_poll(&s->protocol, &s->transport, now);
    }
}
void uc_usb_set_rumble_enabled(int enabled)
{
    usb.rumble = enabled != 0;
    if (!usb.rumble)
        for (unsigned i = 0; i < usb.count; i++)
            stop_motors(&usb.slots[i], usb.clock);
}
static void close_source(usb_slot *s, uint64_t now)
{
    stop_motors(s, now);
    if (s->transport_live && transport_valid(&s->transport))
        s->transport.ops->close(s->transport.context);
    s->transport_live = s->attached = s->valid = s->health_valid = 0;
    s->rumble_test_seen = 0;
    s->submitted = 0;
    memset(&s->protocol, 0, sizeof(s->protocol));
}
static int cleanup_output(usb_slot *s)
{
    if (s->session) {
        int result = uc_core_disable_session(s->session);
        if (result && result != -2) {
            s->cleanup = 1;
            return 2;
        }
    }
    s->session = s->sequence = s->submitted = 0;
    s->pending = s->cleanup = 0;
    return 0;
}
static void release_slot(usb_slot *s)
{
    if (s->native_lease) {
        uc_usb_transport_release(&s->transport);
        s->native_lease = 0;
    }
}
static void fail_slot(unsigned index, const char *reason, int rescan, uint64_t now)
{
    usb_slot *s = &usb.slots[index];
    s->reason = reason;
    log_code(1, index, reason);
    close_source(s, now);
    s->last_scan = now;
    s->scanned = 1;
    if (!rescan) {
        s->user = -1;
        s->assigning = 0;
        s->failed = 1;
        usb.reason = reason;
    }
    (void)cleanup_output(s);
}
int uc_usb_running(void)
{
    if (usb.enabled)
        return 1;
    for (unsigned i = 0; i < usb.count; i++)
        if (usb.slots[i].session || usb.slots[i].cleanup || usb.slots[i].attached)
            return 1;
    return 0;
}
int uc_usb_set_transports(const uc_usb_transport *transports, size_t count)
{
    if (uc_usb_running())
        return 2;
    if (count > USB_SLOTS || (count && !transports))
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (!transport_valid(&transports[i]))
            return -1;
        for (size_t j = 0; j < i; j++)
            if (transports[i].ops == transports[j].ops &&
                transports[i].context == transports[j].context)
                return -1;
    }
    memset(usb.injected, 0, sizeof(usb.injected));
    if (count)
        memcpy(usb.injected, transports, count * sizeof(*transports));
    usb.injected_count = (unsigned)count;
    return 0;
}
int uc_usb_set_transport(const uc_usb_transport *transport)
{
    return uc_usb_set_transports(transport, transport ? 1 : 0);
}
int uc_usb_start(int32_t user)
{
    if (uc_usb_running())
        return 2;
    if (user <= 0 || !uc_backend_can_start(user)) {
        usb.reason = "start_not_ready";
        return -1;
    }
    memset(usb.slots, 0, sizeof(usb.slots));
    usb.rejected_count = 0;
    usb.count = 0;
    unsigned wanted = usb.injected_count ? usb.injected_count : USB_SLOTS;
    for (unsigned i = 0; i < wanted; i++) {
        usb_slot *s = &usb.slots[i];
        s->user = -1;
        if (usb.injected_count)
            s->transport = usb.injected[i];
        else {
            if (!uc_usb_transport_open(&s->transport))
                break;
            s->native_lease = 1;
        }
        if (!transport_valid(&s->transport)) {
            release_slot(s);
            break;
        }
        s->transport_live = 1;
        ++usb.count;
    }
    if (!usb.count) {
        usb.reason = "transport_unsupported_or_open_failed";
        return -1;
    }
    usb.slots[0].user = user;
    usb.enabled = 1;
    usb.reason = NULL;
    usb.clock_valid = usb.presence_valid = 0;
    uc_log_info("[usb] started slots=%u user=%d; XInput and verified Generic HID inputs", usb.count,
                user);
    return 0;
}
int uc_usb_stop(void)
{
    usb.enabled = 0;
    uc_log_info("[usb] stopping and releasing owned interfaces");
    int result = 0;
    for (unsigned i = 0; i < usb.count; i++) {
        usb_slot *s = &usb.slots[i];
        close_source(s, usb.clock);
        release_slot(s);
        s->assigning = 0;
        if (cleanup_output(s))
            result = 2;
        else
            s->user = -1;
    }
    return result;
}
void uc_usb_suspend(void)
{
    usb.reason = "suspended_manual_restart_required";
    (void)uc_usb_stop();
}
int uc_usb_assign(unsigned slot, const char *node, int32_t user)
{
    if (!usb.enabled || slot >= usb.count || !node || !usb.slots[slot].attached ||
        strcmp(node, usb.slots[slot].device.node) || user <= 0 || !uc_backend_can_start(user))
        return -1;
    usb_slot *s = &usb.slots[slot];
    for (unsigned i = 0; i < usb.count; i++)
        if (i != slot && (usb.slots[i].user == user ||
                          (usb.slots[i].assigning && usb.slots[i].next_user == user)))
            return -1;
    if (!s->assigning && s->user == user)
        return 0;
    stop_motors(s, usb.clock);
    s->assigning = 1;
    s->next_user = user;
    s->valid = 0;
    if (cleanup_output(s))
        return 2;
    s->user = user;
    s->assigning = 0;
    s->reason = NULL;
    return 0;
}
static int16_t axis(uint8_t value)
{
    return value >= 128 ? (int16_t)(((int)value - 128) * 32767 / 127)
                        : (int16_t)(((int)value - 128) * 256);
}
static int submit(usb_slot *s, uint64_t now)
{
    if (!s->valid || s->submitted == s->frames || !active(s))
        return 0;
    if (now < s->latest_at || now - s->latest_at > 500 || s->sequence == UINT64_MAX)
        return -1;
    pad_state *p = &s->latest;
    int16_t axes[4] = {axis(p->lx), axis(p->ly), axis(p->rx), axis(p->ry)};
    uint16_t triggers[2] = {(uint16_t)(p->l2 * 257u), (uint16_t)(p->r2 * 257u)};
    if (uc_core_submit(s->session, ++s->sequence,
                       uc_device_options_buttons(p->buttons, s->options.swap_ab), axes, triggers,
                       now))
        return -1;
    s->submitted = s->frames;
    return 0;
}
static void record_rejection(const uc_usb_device *device)
{
    for (unsigned i = 0; i < usb.rejected_count; i++)
        if (!strcmp(usb.rejected[i].node, device->node)) {
            if (usb.rejected[i].vid == device->vid && usb.rejected[i].pid == device->pid &&
                usb.rejected[i].error_number == device->error_number &&
                !strcmp(usb.rejected[i].reason, device->reason) &&
                usb.rejected[i].diagnostic.config_hash == device->diagnostic.config_hash &&
                usb.rejected[i].diagnostic.report_errno == device->diagnostic.report_errno &&
                usb.rejected[i].diagnostic.report_saved == device->diagnostic.report_saved &&
                usb.rejected[i].diagnostic.report_valid == device->diagnostic.report_valid &&
                !memcmp(usb.rejected[i].diagnostic.report, device->diagnostic.report,
                        sizeof(device->diagnostic.report)))
                return;
            usb.rejected[i] = *device;
            goto changed;
        }
    if (usb.rejected_count == 16) {
        memmove(usb.rejected, usb.rejected + 1, 15 * sizeof(usb.rejected[0]));
        --usb.rejected_count;
    }
    usb.rejected[usb.rejected_count++] = *device;
changed:
    uc_log_warn("[usb] node=%s vid=%04x pid=%04x rejected=%s errno=%d", device->node, device->vid,
                device->pid, device->reason, device->error_number);
    uc_log_info("[usb] node=%s USB_GET_DEVICE_DESC attempted=%d ok=%d errno=%d; "
                "USB_GET_DEVICEINFO attempted=%d ok=%d errno=%d",
                device->node, device->diagnostic.descriptor_attempted,
                device->diagnostic.descriptor_ok, device->diagnostic.descriptor_errno,
                device->diagnostic.info_attempted, device->diagnostic.info_ok,
                device->diagnostic.info_errno);
    uc_log_info("[usb] node=%s protocol=%s interfaces=%s config_valid=%d report=%u/%u errno=%d",
                device->node, uc_usb_diagnostic_hint(device->vid, device->pid, &device->diagnostic),
                device->diagnostic.summary, device->diagnostic.config_valid,
                device->diagnostic.report_saved, device->diagnostic.report_declared,
                device->diagnostic.report_errno);
}
static void forget_rejection(const char *node)
{
    for (unsigned i = 0; i < usb.rejected_count; i++)
        if (!strcmp(usb.rejected[i].node, node)) {
            memmove(usb.rejected + i, usb.rejected + i + 1,
                    (usb.rejected_count - i - 1) * sizeof(usb.rejected[0]));
            --usb.rejected_count;
            return;
        }
}
static void poll_slot(unsigned index, uint64_t now)
{
    usb_slot *s = &usb.slots[index];
    if (s->cleanup) {
        if (cleanup_output(s))
            return;
        if (s->assigning) {
            s->user = s->next_user;
            s->assigning = 0;
            s->valid = 0;
        }
        if (!usb.enabled) {
            s->user = -1;
            return;
        }
    }
    if (!usb.enabled || s->failed)
        return;
    if (s->user > 0 && !uc_backend_can_start(s->user)) {
        fail_slot(index, "power_or_user_unavailable", 0, now);
        return;
    }
    if (!s->attached) {
        if (s->scanned && now - s->last_scan < 100)
            return;
        s->scanned = 1;
        s->last_scan = now;
        s->transport_live = 1;
        uc_usb_device device = {0};
        int found = s->transport.ops->scan(s->transport.context, &device);
        if (found < 0) {
            fail_slot(index, "scan_failed", 0, now);
            return;
        }
        if (!found)
            return;
        if (found == 2) {
            device.node[sizeof(device.node) - 1] = 0;
            device.reason[sizeof(device.reason) - 1] = 0;
            record_rejection(&device);
            return;
        }
        forget_rejection(device.node);
        s->device = device;
        s->device.node[sizeof(s->device.node) - 1] = 0;
        for (size_t i = 0; s->device.node[i]; i++) {
            unsigned char c = (unsigned char)s->device.node[i];
            if (c < 32 || c >= 127 || c == '"' || c == '\\')
                s->device.node[i] = '_';
        }
        snprintf(s->options_key, sizeof s->options_key, "usb:%s:%04X:%04X", s->device.node,
                 s->device.vid, s->device.pid);
        s->known = profile_device_known(PAD_TRANSPORT_USB, device.vid, device.pid);
        s->options = uc_device_options_get(s->options_key, s->known);
        s->attached = 1;
        if (device.kind != UC_USB_XINPUT && device.kind != UC_USB_GENERIC_HID) {
            fail_slot(index, "unsupported_device", 0, now);
            return;
        }
        s->valid = s->health_valid = 0;
        s->frames = s->submitted = 0;
        s->reason = NULL;
        uc_usb_protocol_begin(&s->protocol, device.kind, now);
        if (device.kind == UC_USB_GENERIC_HID &&
            uc_usb_protocol_hid_descriptor(&s->protocol, &device)) {
            fail_slot(index, "hid_descriptor_unsupported", 0, now);
            return;
        }
        uc_log_info("[usb] device options key=%s known=%d rumble=%d swap_ab=%d", s->options_key,
                    s->known, s->options.rumble, s->options.swap_ab);
        log_code(3, index, "device_opened");
    }
    if (!s->health_valid || now - s->health_at >= 100) {
        int alive = s->transport.ops->alive(s->transport.context);
        if (alive <= 0) {
            fail_slot(index, alive ? "transport_health_failed" : "device_disconnected", 1, now);
            return;
        }
        s->health_at = now;
        s->health_valid = 1;
    }
    if (s->rumble_test_until && now >= s->rumble_test_until) {
        s->rumble_test_until = 0;
        (void)uc_usb_protocol_rumble(&s->protocol, 0, 0, now);
        uc_log_debug("[usb] rumble test ended key=%s", s->options_key);
    } else if (s->rumble_test_until) {
        (void)uc_usb_protocol_rumble(&s->protocol, 200, 200, now);
    } else if (active(s) && usb.rumble && s->options.rumble && s->device.kind == UC_USB_XINPUT) {
        uint8_t strong = 0, weak = 0;
        int feedback = uc_core_read_rumble(s->session, now, &strong, &weak);
        if (feedback > 0)
            (void)uc_usb_protocol_rumble(&s->protocol, strong, weak, now);
        else if (feedback < 0)
            (void)uc_usb_protocol_rumble(&s->protocol, 0, 0, now);
    }
    if (uc_usb_protocol_poll(&s->protocol, &s->transport, now)) {
        fail_slot(index, "initialization_or_output_failed", 0, now);
        return;
    }
    for (unsigned reads = 0; reads < 8; reads++) {
        uint8_t packet[512];
        int count = s->transport.ops->read(s->transport.context, packet, sizeof(packet));
        if (count < 0 || count > (int)sizeof(packet)) {
            fail_slot(index, "transport_read_failed", 1, now);
            return;
        }
        if (!count)
            break;
        uc_usb_diagnostics *diag = &s->device.diagnostic;
        if (!diag->first_input_length) {
            diag->first_input_length = (unsigned)count;
            diag->first_input_saved = (unsigned)count < sizeof(diag->first_input)
                                          ? (unsigned)count
                                          : sizeof(diag->first_input);
            memcpy(diag->first_input, packet, diag->first_input_saved);
        }
        pad_state state = {0};
        int valid = uc_usb_protocol_feed(&s->protocol, packet, (size_t)count, &state, now);
        if (valid < 0) {
            fail_slot(index, "protocol_failed", 0, now);
            return;
        }
        if (valid && s->protocol.ready) {
            if (s->frames == UINT64_MAX) {
                fail_slot(index, "input_counter_exhausted", 0, now);
                return;
            }
            s->latest = state;
            s->latest_at = now;
            s->valid = 1;
            ++s->frames;
        }
    }
    if (s->session && !s->pending && !active(s)) {
        fail_slot(index, "session_lost", 0, now);
        return;
    }
    if (s->assigning || s->user <= 0 || !s->protocol.ready || !s->valid)
        return;
    if (!s->session || s->pending) {
        if (!s->pending && (now < s->latest_at || now - s->latest_at > 500))
            return;
        uint64_t session = s->pending ? s->session : 0;
        int result = uc_core_enable_physical(s->user, now, &session);
        if (result && session)
            s->session = session;
        if (result == 2) {
            if (session) {
                s->session = session;
                s->pending = 1;
            } else
                s->reason = "user_or_capacity_busy";
            return;
        }
        if (result || !session) {
            fail_slot(index, "virtual_enable_failed", 0, now);
            return;
        }
        s->session = session;
        s->pending = 0;
        s->reason = NULL;
        s->sequence = s->submitted = 0;
        if (now < s->latest_at || now - s->latest_at > 500) {
            s->valid = 0;
            s->submitted = s->frames;
            return;
        }
    }
    if (submit(s, now)) {
        fail_slot(index, "input_submit_failed", 0, now);
        return;
    }
    if (s->submitted && s->submitted == s->frames && active(s) && s->health_valid &&
        now >= s->health_at && now - s->health_at <= 100 &&
        !profile_usb_input_watchdog(s->device.vid, s->device.pid)) {
        if (uc_core_refresh_source(s->session, now))
            fail_slot(index, "source_lease_expired", 0, now);
    }
}
void uc_usb_poll(uint64_t now)
{
    if (usb.clock_valid && uc_usb_running() && (now < usb.clock || now - usb.clock > 500)) {
        usb.clock = now;
        usb.reason = "poll_gap_manual_restart_required";
        (void)uc_usb_stop();
        return;
    }
    usb.clock = now;
    usb.clock_valid = 1;
    if (usb.enabled && (!usb.presence_valid || now - usb.last_presence >= 100)) {
        usb.presence_valid = 1;
        usb.last_presence = now;
        char node[24];
        if (uc_usb_transport_take_removed(node, sizeof(node))) {
            forget_rejection(node);
            uc_log_info("[usb] node=%s removed diagnostic snapshot after HID unplug/replacement",
                        node);
        }
    }
    for (unsigned i = 0; i < usb.count; i++)
        poll_slot(i, now);
}
static int append(char *b, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap)
        return -1;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(b + *used, cap - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}
static int diagnostic_fields(char *b, size_t cap, size_t *used, const uc_usb_device *device,
                             int full)
{
    const uc_usb_diagnostics *d = &device->diagnostic;
#define FIELD(...)                                                                                 \
    do {                                                                                           \
        if (append(b, cap, used, __VA_ARGS__))                                                     \
            return -1;                                                                             \
    } while (0)
    FIELD(",\"device_descriptor_attempted\":%s,\"device_descriptor_ok\":%s,"
          "\"device_descriptor_errno\":%d,\"device_info_attempted\":%s,"
          "\"device_info_ok\":%s,\"device_info_errno\":%d",
          d->descriptor_attempted ? "true" : "false", d->descriptor_ok ? "true" : "false",
          d->descriptor_errno, d->info_attempted ? "true" : "false", d->info_ok ? "true" : "false",
          d->info_errno);
    FIELD(",\"output_endpoint_available\":%s,\"output_endpoint_errno\":%d",
          d->output_available ? "true" : "false", d->output_errno);
    FIELD(",\"protocol_hint\":\"%s\",\"interface_summary\":\"%s\",\"device_class\":%u,"
          "\"config_index\":%u,\"config_value\":%u,\"config_length\":%u,\"config_valid\":%s,"
          "\"hid_report_interface\":%d,\"hid_report_declared\":%u,\"hid_report_captured\":%u,"
          "\"hid_report_attempted\":%s,\"hid_report_errno\":%d,\"hid_report_valid\":%s,"
          "\"hid_gamepad\":%s,\"hid_joystick\":%s,\"hid_keyboard\":%s,\"hid_mouse\":%s",
          uc_usb_diagnostic_hint(device->vid, device->pid, d), d->summary, d->device_class,
          d->config_index, d->config_value, d->config_length, d->config_valid ? "true" : "false",
          d->report_interface, d->report_declared, d->report_saved,
          d->report_attempted ? "true" : "false", d->report_errno,
          d->report_valid ? "true" : "false", d->gamepad ? "true" : "false",
          d->joystick ? "true" : "false", d->keyboard ? "true" : "false",
          d->mouse ? "true" : "false");
    FIELD(",\"detach_attempted\":%s,\"driver_detached\":%s,\"detach_errno\":%d,"
          "\"claim_attempted\":%s,\"interface_claimed\":%s,\"claim_errno\":%d,"
          "\"first_input_length\":%u",
          d->detach_attempted ? "true" : "false", d->detached ? "true" : "false", d->detach_errno,
          d->claim_attempted ? "true" : "false", d->claimed ? "true" : "false", d->claim_errno,
          d->first_input_length);
    if (!full)
        return 0;
    FIELD(",\"interfaces_truncated\":%s,\"endpoints_truncated\":%s,\"interfaces\":[",
          d->interfaces_truncated ? "true" : "false", d->endpoints_truncated ? "true" : "false");
    for (unsigned i = 0; i < d->interface_count && i < UC_USB_DIAG_INTERFACES; i++) {
        const uc_usb_interface_info *p = &d->interfaces[i];
        FIELD("%s{\"number\":%u,\"index\":%u,\"alternate\":%u,\"class\":%u,\"subclass\":%u,"
              "\"protocol\":%u,\"endpoints\":%u,\"hid_report_length\":%u}",
              i ? "," : "", p->number, p->index, p->alternate, p->class_code, p->subclass,
              p->protocol, p->endpoints, p->report_length);
    }
    FIELD("],\"endpoints\":[");
    for (unsigned i = 0; i < d->endpoint_count && i < UC_USB_DIAG_ENDPOINTS; i++) {
        const uc_usb_endpoint_info *p = &d->endpoints[i];
        FIELD("%s{\"interface\":%u,\"alternate\":%u,\"address\":%u,\"attributes\":%u,"
              "\"packet_size\":%u,\"interval\":%u}",
              i ? "," : "", p->interface_number, p->alternate, p->address, p->attributes,
              p->packet_size, p->interval);
    }
    FIELD("],\"config_descriptor_truncated\":%s,\"report_descriptor_truncated\":%s,"
          "\"config_descriptor_hex\":\"",
          d->config_saved < d->config_length ? "true" : "false",
          d->report_saved < d->report_declared ? "true" : "false");
    for (unsigned i = 0; i < d->config_saved && i < sizeof(d->config); i++)
        FIELD("%02x", d->config[i]);
    FIELD("\",\"report_descriptor_hex\":\"");
    for (unsigned i = 0; i < d->report_saved && i < sizeof(d->report); i++)
        FIELD("%02x", d->report[i]);
    FIELD("\",\"first_input_hex\":\"");
    for (unsigned i = 0; i < d->first_input_saved && i < sizeof(d->first_input); i++)
        FIELD("%02x", d->first_input[i]);
    FIELD("\"");
#undef FIELD
    return 0;
}
size_t uc_usb_diagnostics_json(char *buffer, size_t capacity)
{
    if (!buffer || !capacity)
        return 0;
    size_t used = 0;
    buffer[0] = 0;
    if (append(buffer, capacity, &used, "{\"schema_version\":1,\"snapshot\":true,\"devices\":["))
        return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < usb.count + usb.rejected_count; i++) {
        int opened = i < usb.count;
        const uc_usb_device *d = opened ? &usb.slots[i].device : &usb.rejected[i - usb.count];
        if (!d->node[0])
            continue;
        if (append(buffer, capacity, &used,
                   "%s{\"node\":\"%s\",\"vid\":\"%04X\",\"pid\":\"%04X\",\"opened\":%s,"
                   "\"reason\":\"%s\",\"errno\":%d",
                   count++ ? "," : "", d->node, d->vid, d->pid,
                   opened && usb.slots[i].attached ? "true" : "false",
                   opened && usb.slots[i].reason ? usb.slots[i].reason : d->reason,
                   d->error_number) ||
            diagnostic_fields(buffer, capacity, &used, d, 1) ||
            append(buffer, capacity, &used, "}")) {
            buffer[0] = 0;
            return 0;
        }
    }
    if (append(buffer, capacity, &used, "]}")) {
        buffer[0] = 0;
        return 0;
    }
    return used;
}
size_t uc_usb_status_json(char *buffer, size_t capacity)
{
    if (!buffer || !capacity)
        return 0;
    size_t used = 0;
    buffer[0] = 0;
    int bound = 0, pending = 0;
    int32_t user = -1;
    for (unsigned i = 0; i < usb.count; i++) {
        bound |= active(&usb.slots[i]);
        pending |= usb.slots[i].pending;
        if (i == 0)
            user = usb.slots[i].user;
    }
#define ADD(...)                                                                                   \
    do {                                                                                           \
        if (append(buffer, capacity, &used, __VA_ARGS__)) {                                        \
            buffer[0] = 0;                                                                         \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)
    ADD("{\"state\":\"%s\",\"reason\":\"%s\",\"user_id\":%d,\"pending\":%s,\"bound\":%s,"
        "\"devices\":[",
        usb.enabled  ? "on"
        : usb.reason ? "failed"
                     : "off",
        usb.reason ? usb.reason : "", user, pending ? "true" : "false", bound ? "true" : "false");
    int comma = 0;
    for (unsigned i = 0; i < usb.count; i++) {
        usb_slot *s = &usb.slots[i];
        if (!s->device.node[0])
            continue;
        const char *profile =
            s->device.kind == UC_USB_GENERIC_HID ? "generic-hid-usb" : "xinput-usb";
        const char *model = pad_display_name_for_transport(PAD_TRANSPORT_USB, s->device.vid,
                                                           s->device.pid, profile);
        /* Model aliases are ASCII but may contain quotes: quote them as JSON. */
        char escaped[128];
        size_t at = 0;
        for (size_t n = 0; model[n] && at + 2 < sizeof(escaped); n++) {
            if (model[n] == '"' || model[n] == '\\')
                escaped[at++] = '\\';
            escaped[at++] = model[n];
        }
        escaped[at] = 0;
        ADD("%s{\"slot\":%u,\"node\":\"%s\",\"vid\":\"%04X\",\"pid\":\"%04X\",\"profile\":\"%s\","
            "\"model\":\"%s\",\"user_id\":%d,\"connected\":%s,\"input_frames\":%llu,\"bound\":%s,"
            "\"pending\":%s,\"rumble_available\":%s,\"session_id\":%llu,\"reason\":\"%s\"",
            comma++ ? "," : "", i, s->device.node, s->device.vid, s->device.pid, profile, escaped,
            s->user, s->attached ? "true" : "false", (unsigned long long)s->frames,
            active(s) ? "true" : "false", s->pending ? "true" : "false",
            s->device.kind == UC_USB_XINPUT && s->protocol.ready && !s->protocol.rumble_failed
                ? "true"
                : "false",
            (unsigned long long)s->session, s->reason ? s->reason : "");
        ADD(",\"settings_key\":\"%s\",\"rumble\":%s,\"swap_ab\":%s,\"known_device\":%s",
            s->options_key, s->options.rumble ? "true" : "false",
            s->options.swap_ab ? "true" : "false", s->known ? "true" : "false");
        ADD(",\"rumble_enabled\":%s", usb.rumble && s->options.rumble ? "true" : "false");
        if (diagnostic_fields(buffer, capacity, &used, &s->device, 0)) {
            buffer[0] = 0;
            return 0;
        }
        ADD("}");
    }
    ADD("],\"rejected\":[");
    for (unsigned i = 0; i < usb.rejected_count; i++) {
        const uc_usb_device *d = &usb.rejected[i];
        ADD("%s{\"node\":\"%s\",\"vid\":\"%04X\",\"pid\":\"%04X\","
            "\"reason\":\"%s\",\"errno\":%d",
            i ? "," : "", d->node, d->vid, d->pid, d->reason, d->error_number);
        if (diagnostic_fields(buffer, capacity, &used, d, 0)) {
            buffer[0] = 0;
            return 0;
        }
        ADD("}");
    }
    ADD("]}");
#undef ADD
    return used;
}

int uc_usb_device_options_match(const char *key, uint64_t session)
{
    for (unsigned i = 0; i < usb.count; ++i) {
        usb_slot *s = &usb.slots[i];
        if (s->attached && s->session == session && !strcmp(s->options_key, key))
            return 1;
    }
    return 0;
}
void uc_usb_device_options_changed(void)
{
    for (unsigned i = 0; i < usb.count; ++i) {
        usb_slot *s = &usb.slots[i];
        if (!s->attached)
            continue;
        uc_device_options next = uc_device_options_get(s->options_key, s->known);
        if (s->options.rumble && !next.rumble)
            stop_motors(s, usb.clock);
        if (s->options.swap_ab != next.swap_ab && usb.clock >= s->latest_at &&
            usb.clock - s->latest_at < 500)
            s->submitted = 0;
        s->options = next;
    }
}

int uc_usb_test_rumble(const char *key, uint64_t session, uint64_t now)
{
    if (!key || !usb.enabled || !usb.rumble || now > UINT64_MAX - 500)
        return -1;
    for (unsigned i = 0; i < usb.count; ++i) {
        usb_slot *s = &usb.slots[i];
        if (!s->attached || s->session != session || strcmp(s->options_key, key))
            continue;
        if (!s->options.rumble || s->device.kind != UC_USB_XINPUT || !s->protocol.ready ||
            s->protocol.failed || s->protocol.rumble_failed)
            return -1;
        if (s->rumble_test_seen && (now < s->rumble_test_last || now - s->rumble_test_last < 1000))
            return 2;
        if (uc_usb_protocol_rumble(&s->protocol, 200, 200, now))
            return -1;
        s->rumble_test_until = now + 500;
        s->rumble_test_last = now;
        s->rumble_test_seen = 1;
        uc_log_info("[usb] rumble test key=%s duration_ms=500 queued=1", key);
        return 0;
    }
    return -1;
}
