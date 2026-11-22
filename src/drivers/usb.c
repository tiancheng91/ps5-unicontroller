#include "../logging.h"
#include "usb.h"
#include "bt.h"
#include "../core/device_options.h"
#include "usb/protocol.h"
#include "../core/uc_core.h"
#include "../ps5/uc_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

#define USB_SLOTS 4u
#define HID_FIRST_INPUT_TIMEOUT_MS 30000u
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
    int scanned, assigning, failed, recycle, first_input_warned;
    uint64_t opened_at;
    int reselecting, saw_login;
    uint64_t next_login_check;
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
    int32_t preferred_user;
    uint64_t clock, last_presence;
    int presence_valid;
    const char *reason;
} usb = {.level = 3, .rumble = 1, .preferred_user = -1};
static int waiting_for_hid_input(const usb_slot *s)
{
    return s->attached && s->device.kind == UC_USB_GENERIC_HID && !s->frames;
}
static uint64_t first_input_remaining(const usb_slot *s, uint64_t now)
{
    uint64_t elapsed = now >= s->opened_at ? now - s->opened_at : 0;
    return elapsed < HID_FIRST_INPUT_TIMEOUT_MS ? HID_FIRST_INPUT_TIMEOUT_MS - elapsed : 0;
}
static int transport_valid(const uc_usb_transport *t)
{
    return t && t->ops && t->ops->scan && t->ops->read && t->ops->write && t->ops->alive &&
           t->ops->close;
}
static int active(const usb_slot *s)
{
    return s->session && uc_core_session_active(s->session);
}
/* Only live sources and outstanding output cleanup reserve an account. */
static int user_reserved(unsigned except, int32_t user)
{
    for (unsigned i = 0; i < usb.count; i++) {
        const usb_slot *other = &usb.slots[i];
        if (i == except)
            continue;
        if ((other->user == user && (other->attached || other->session || other->cleanup)) ||
            (other->assigning && other->next_user == user))
            return 1;
    }
    return 0;
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
    if (s->attached && s->transport_live && s->device.kind == UC_USB_SWITCH_PRO &&
        s->protocol.switch1_timeout_disabled) {
        static const uint8_t timeout_on[] = {0x80, 5};
        int result = s->transport.ops->write(s->transport.context, timeout_on, sizeof timeout_on);
        uc_log_debug("[usb-switch1] restore USB timeout node=%s result=%d; no retry on close",
                     s->device.node, result);
        s->protocol.switch1_timeout_disabled = 0;
    }
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
static void record_rejection(const uc_usb_device *device);
/* Called only after output cleanup; empty scanners must not reserve accounts. */
static void recycle_slot(usb_slot *s, uint64_t now)
{
    uc_usb_transport transport = s->transport;
    int native_lease = s->native_lease;
    memset(s, 0, sizeof *s);
    s->transport = transport;
    s->native_lease = native_lease;
    s->user = -1;
    s->scanned = 1;
    s->last_scan = now;
}
static void fail_slot(unsigned index, const char *reason, int rescan, uint64_t now)
{
    usb_slot *s = &usb.slots[index];
    s->reason = reason;
    log_code(1, index, reason);
    uc_log_debug("[usb] failure slot=%u node=%s vid=%04x pid=%04x user=%d attached=%d "
                 "ready=%d frames=%llu reads=%llu empty=%llu would_block=%llu errors=%llu "
                 "last_errno=%d rescan=%d",
                 index, s->device.node, s->device.vid, s->device.pid, s->user, s->attached,
                 s->protocol.ready, (unsigned long long)s->frames,
                 (unsigned long long)s->device.diagnostic.read_attempts,
                 (unsigned long long)s->device.diagnostic.read_empty,
                 (unsigned long long)s->device.diagnostic.read_would_block,
                 (unsigned long long)s->device.diagnostic.read_errors,
                 s->device.diagnostic.last_read_errno, rescan);
    int recycle = s->attached && strcmp(reason, "power_or_user_unavailable");
    if (recycle && !rescan) {
        uc_usb_device diagnostic = s->device;
        snprintf(diagnostic.reason, sizeof diagnostic.reason, "%s", reason);
        record_rejection(&diagnostic);
        if (!uc_usb_transport_quarantine(&s->transport)) {
            recycle = 0; /* Unknown transport cannot promise per-connection isolation. */
            uc_log_warn("[usb] slot=%u quarantine unavailable; manual USB restart required", index);
        }
    }
    close_source(s, now);
    s->last_scan = now;
    s->scanned = 1;
    s->recycle = recycle;
    if (!rescan && !recycle) {
        s->user = -1;
        s->assigning = 0;
        s->failed = 1;
        usb.reason = reason;
    }
    (void)cleanup_output(s);
    if (recycle && !s->cleanup) {
        recycle_slot(s, now);
        uc_log_info("[usb] slot=%u released; scanner available for other connections", index);
    }
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
    if (user < 0 || (user == 0 ? !uc_backend_transport_ready() : !uc_backend_can_start(user))) {
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
    usb.preferred_user = user;
    usb.enabled = 1;
    usb.reason = NULL;
    usb.clock_valid = usb.presence_valid = 0;
    uc_log_info("[usb] started slots=%u user=%d; XInput, verified Generic HID, Switch Pro and "
                "experimental Switch 2 Pro USB inputs",
                usb.count, user);
    return 0;
}
int uc_usb_stop(void)
{
    usb.enabled = 0;
    usb.preferred_user = -1;
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
    usb.reason = NULL; /* Rest is an expected pause; runtime decides when to restart. */
    (void)uc_usb_stop();
}
int uc_usb_assign(unsigned slot, const char *node, int32_t user)
{
    if (!usb.enabled || slot >= usb.count || !node || !usb.slots[slot].attached ||
        strcmp(node, usb.slots[slot].device.node) || user <= 0 || !uc_backend_can_start(user)) {
        uc_log_debug("[usb] assignment rejected slot=%u count=%u enabled=%d node=%s user=%d "
                     "attached=%d",
                     slot, usb.count, usb.enabled, node ? node : "(null)", user,
                     slot < usb.count ? usb.slots[slot].attached : 0);
        return -1;
    }
    usb_slot *s = &usb.slots[slot];
    if (user_reserved(slot, user))
        return -1;
    usb.preferred_user = user;
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
int uc_usb_reselection_pending(void)
{
    for (unsigned i = 0; i < usb.count; ++i) {
        const usb_slot *s = &usb.slots[i];
        if (s->attached && !s->failed && s->reselecting && !active(s))
            return 1;
    }
    return 0;
}
int uc_usb_reselect_user(unsigned slot, const char *node)
{
    if (!usb.enabled || slot >= usb.count || !node || !usb.slots[slot].attached ||
        strcmp(node, usb.slots[slot].device.node) || !uc_backend_transport_ready())
        return -1;
    usb_slot *s = &usb.slots[slot];
    if (s->assigning)
        return s->next_user == 0 ? 2 : -1;
    if (!uc_core_system_assignment_available(s->session))
        return -3;

    s->saw_login = 0;
    s->next_login_check = 0;
    s->reselecting = 1;
    stop_motors(s, usb.clock);
    s->assigning = 1;
    s->next_user = 0; /* Zero means system choice, never the startup preference. */
    s->valid = 0;
    s->reason = NULL;
    uc_log_info("[usb] slot=%u node=%s user reselection requested; PS5 chooses user, "
                "final ownership unverified",
                slot, node);
    if (cleanup_output(s))
        return 2;
    s->user = 0;
    s->assigning = 0;
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
/* A user-list failure must never be interpreted as everyone signing out. */
static int login_state(void)
{
    int32_t users[4];
    if (uc_backend_users(users))
        return -1;
    for (unsigned i = 0; i < 4; ++i)
        if (users[i] > 0)
            return 1;
    return 0;
}
static void poll_slot(unsigned index, uint64_t now)
{
    usb_slot *s = &usb.slots[index];
    if (s->cleanup) {
        if (cleanup_output(s))
            return;
        if (s->recycle) {
            recycle_slot(s, now);
            uc_log_info("[usb] slot=%u output cleanup complete; scanner reusable", index);
        }
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
    if (s->user == 0 && !uc_backend_transport_ready()) {
        fail_slot(index, "power_or_user_unavailable", 0, now);
        return;
    }
    if (s->user > 0 && !uc_backend_can_start(s->user)) {
        if (!uc_backend_transport_ready()) {
            fail_slot(index, "power_or_user_unavailable", 0, now);
            return;
        }
        /* Account loss must release its virtual output, but a healthy attached
         * external device can remain unassigned for an explicit user choice.
         * Never silently transfer input to another signed-in account. */
        uc_log_info("[usb] slot=%u user=%d unavailable; releasing output, retaining transport for "
                    "reassignment",
                    index, s->user);
        stop_motors(s, now);
        if (usb.preferred_user == s->user)
            usb.preferred_user = -1;
        s->saw_login = 1; /* Its explicitly assigned account was previously live. */
        s->user = s->next_user = -1;
        s->assigning = s->valid = 0;
        s->reason = "user_unavailable";
        if (cleanup_output(s))
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
        s->opened_at = now;
        s->first_input_warned = 0;
        if (device.kind != UC_USB_XINPUT && device.kind != UC_USB_GENERIC_HID &&
            device.kind != UC_USB_SWITCH2_PRO && device.kind != UC_USB_SWITCH_PRO) {
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
        if (device.kind == UC_USB_GENERIC_HID)
            uc_log_info("[usb] node=%s waiting for valid HID input; press a button or move a stick "
                        "within %u ms",
                        device.node, HID_FIRST_INPUT_TIMEOUT_MS);
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
    if (s->user == 0 && active(s) && !s->saw_login && now >= s->next_login_check) {
        s->next_login_check = now + 1000;
        s->saw_login = login_state() == 1;
    }
    if (s->rumble_test_until && now >= s->rumble_test_until) {
        s->rumble_test_until = 0;
        (void)uc_usb_protocol_rumble(&s->protocol, 0, 0, now);
        uc_log_debug("[usb] rumble test ended key=%s", s->options_key);
    } else if (s->rumble_test_until) {
        (void)uc_usb_protocol_rumble(&s->protocol, 200, 200, now);
    } else if (active(s) && usb.rumble && s->options.rumble &&
               uc_usb_rumble_available(&s->protocol)) {
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
        uc_usb_diagnostics *diag = &s->device.diagnostic;
        if (diag->read_attempts != UINT64_MAX)
            ++diag->read_attempts;
        errno = 0;
        int count = s->transport.ops->read(s->transport.context, packet, sizeof(packet));
        diag->last_read_errno = count <= 0 ? errno : 0;
        if (!diag->first_input_length && now >= s->opened_at)
            diag->first_input_wait_ms = now - s->opened_at;
        if (count < 0 || count > (int)sizeof(packet)) {
            if (diag->read_errors != UINT64_MAX)
                ++diag->read_errors;
            fail_slot(index, "transport_read_failed", 1, now);
            return;
        }
        if (!count) {
            uint64_t *counter = errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
                                    ? &diag->read_would_block
                                    : &diag->read_empty;
            if (*counter != UINT64_MAX)
                ++*counter;
            break;
        }
        if (UINT64_MAX - diag->input_bytes >= (unsigned)count)
            diag->input_bytes += (unsigned)count;
        if (!diag->first_input_length) {
            diag->first_input_length = (unsigned)count;
            diag->first_input_saved = (unsigned)count < sizeof(diag->first_input)
                                          ? (unsigned)count
                                          : sizeof(diag->first_input);
            memcpy(diag->first_input, packet, diag->first_input_saved);
            uc_log_debug(
                "[usb] first transport input node=%s bytes=%d waited_ms=%llu attempts=%llu",
                s->device.node, count, (unsigned long long)diag->first_input_wait_ms,
                (unsigned long long)diag->read_attempts);
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
            int ps_pressed = (state.buttons & PAD_PS) && !(s->latest.buttons & PAD_PS);
            s->latest = state;
            s->latest_at = now;
            s->valid = 1;
            ++s->frames;
            if (ps_pressed && s->saw_login && !uc_usb_reselection_pending() &&
                !uc_bt_reselection_pending() && login_state() == 0 &&
                uc_usb_reselect_user(index, s->device.node) >= 0) {
                uc_log_info(
                    "[usb] slot=%u PS pressed after logout; requesting system user selection",
                    index);
                return;
            }
        }
    }
    if (waiting_for_hid_input(s) && !first_input_remaining(s, now)) {
        fail_slot(index, "hid_first_input_timeout", 0, now);
        return;
    }
    if (!s->device.diagnostic.first_input_length && !s->first_input_warned && now >= s->opened_at &&
        now - s->opened_at >= 5000) {
        s->first_input_warned = 1;
        uc_log_warn("[usb] no first input after 5000ms node=%s vid=%04x pid=%04x "
                    "IN=%#x OUT=%#x reads=%llu empty=%llu would_block=%llu last_errno=%d; "
                    "waiting for valid input",
                    s->device.node, s->device.vid, s->device.pid, s->device.ep_in, s->device.ep_out,
                    (unsigned long long)s->device.diagnostic.read_attempts,
                    (unsigned long long)s->device.diagnostic.read_empty,
                    (unsigned long long)s->device.diagnostic.read_would_block,
                    s->device.diagnostic.last_read_errno);
    }
    if (s->session && !s->pending && !active(s)) {
        fail_slot(index, "session_lost", 0, now);
        return;
    }
    if (!s->assigning && s->user < 0 && s->protocol.ready && s->valid && now >= s->latest_at &&
        now - s->latest_at <= 500 && (!s->reason || strcmp(s->reason, "user_unavailable")) &&
        usb.preferred_user >= 0) {
        if (usb.preferred_user == 0 && uc_backend_transport_ready()) {
            s->user = 0;
            uc_log_info("[usb] slot=%u node=%s registering for PS5 user selection", index,
                        s->device.node);
        } else if (!uc_backend_can_start(usb.preferred_user)) {
            uc_log_info("[usb] preferred user=%d unavailable; explicit selection required",
                        usb.preferred_user);
            usb.preferred_user = -1;
        } else if (!user_reserved(index, usb.preferred_user)) {
            s->user = usb.preferred_user;
            uc_log_info("[usb] slot=%u node=%s selected user=%d; registering virtual output", index,
                        s->device.node, s->user);
        }
    }
    if (s->assigning || s->user < 0 || !s->protocol.ready || !s->valid)
        return;
    if (!s->session || s->pending) {
        if (!s->pending && (now < s->latest_at || now - s->latest_at > 500))
            return;
        uint64_t session = s->pending ? s->session : 0;
        int result = s->user == 0 ? uc_core_enable_reselected(now, &session)
                                  : uc_core_enable_physical(s->user, now, &session);
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
        s->pending = s->reselecting = 0;
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
        now >= s->health_at && now - s->health_at <= 100 && s->device.kind != UC_USB_SWITCH2_PRO &&
        s->device.kind != UC_USB_SWITCH_PRO &&
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
    if (usb.enabled && usb.preferred_user > 0 && !uc_backend_can_start(usb.preferred_user)) {
        uc_log_info("[usb] preferred user=%d unavailable; explicit selection required",
                    usb.preferred_user);
        usb.preferred_user = -1;
    }
    if (usb.enabled && (!usb.presence_valid || now - usb.last_presence >= 100)) {
        usb.presence_valid = 1;
        usb.last_presence = now;
        char node[24];
        if (uc_usb_transport_take_removed(node, sizeof(node))) {
            forget_rejection(node);
            uc_log_info("[usb] node=%s removed diagnostic snapshot after USB unplug/replacement",
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
    FIELD(",\"read_attempts\":%llu,\"read_empty\":%llu,\"read_would_block\":%llu,"
          "\"read_errors\":%llu,\"input_bytes\":%llu,\"last_read_errno\":%d,"
          "\"first_input_wait_ms\":%llu",
          (unsigned long long)d->read_attempts, (unsigned long long)d->read_empty,
          (unsigned long long)d->read_would_block, (unsigned long long)d->read_errors,
          (unsigned long long)d->input_bytes, d->last_read_errno,
          (unsigned long long)d->first_input_wait_ms);
    if (device->kind == UC_USB_SWITCH2_PRO)
        FIELD(",\"configuration_interface\":%u,\"configuration_index\":%u,\"configuration_in\":%u,"
              "\"configuration_out\":%u",
              device->config_iface, device->config_index, device->config_in, device->config_out);
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
    int32_t user = usb.preferred_user;
    for (unsigned i = 0; i < usb.count; i++) {
        bound |= active(&usb.slots[i]);
        pending |= usb.slots[i].pending;
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
        const char *profile = s->device.kind == UC_USB_GENERIC_HID   ? "generic-hid-usb"
                              : s->device.kind == UC_USB_SWITCH2_PRO ? "switch2-pro-usb"
                              : s->device.kind == UC_USB_SWITCH_PRO  ? "switch-pro-usb"
                                                                     : "xinput-usb";
        const char *model = s->device.kind == UC_USB_SWITCH2_PRO
                                ? "Nintendo Switch 2 Pro Controller"
                                : pad_display_name_for_transport(PAD_TRANSPORT_USB, s->device.vid,
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
            active(s) && s->user > 0 ? "true" : "false",
            s->pending || s->assigning ? "true" : "false",
            uc_usb_rumble_available(&s->protocol) ? "true" : "false",
            (unsigned long long)s->session, s->reason ? s->reason : "");
        ADD(",\"system_assignment\":%s,\"output_active\":%s",
            s->user == 0 || (s->assigning && s->next_user == 0) ? "true" : "false",
            active(s) ? "true" : "false");
        int waiting = waiting_for_hid_input(s);
        ADD(",\"waiting_for_input\":%s,\"first_input_remaining_ms\":%llu",
            waiting ? "true" : "false",
            (unsigned long long)(waiting ? first_input_remaining(s, usb.clock) : 0));
        ADD(",\"settings_key\":\"%s\",\"rumble\":%s,\"swap_ab\":%s,\"known_device\":%s",
            s->options_key, s->options.rumble ? "true" : "false",
            s->options.swap_ab ? "true" : "false", s->known ? "true" : "false");
        ADD(",\"rumble_enabled\":%s", usb.rumble && s->options.rumble ? "true" : "false");
        ADD(",\"experimental\":%s", s->device.kind == UC_USB_SWITCH2_PRO ? "true" : "false");
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
        if (!s->options.rumble || !uc_usb_rumble_available(&s->protocol))
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
