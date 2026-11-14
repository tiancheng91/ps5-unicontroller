#include "../logging.h"
/* Per-device physical sessions; only the first source inherits the start account. */
#include "bt.h"
#include "../core/device_options.h"
#include "common/log.h"
#include "protocols/profiles.h"
#include "../core/uc_core.h"
#include "../ps5/uc_backend.h"
#ifdef UC_PS5
#include "bluetooth/hci_usb.h"
#include "common/util.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

/* Boolean gate: signed-in user, stable working power and no foreign output. */
extern int uc_backend_can_start(int32_t user);
typedef struct {
    int connected, valid;
    char options_key[UC_DEVICE_KEY_MAX];
    int known;
    uc_device_options options;
    pad_transport transport;
    uint16_t vid, pid;
    unsigned char addr[6];
    char profile[64];
    pad_state latest;
    uint64_t last_input, frames, connected_at, rumble_test_last;
    int rumble_test_seen;
    int notice_pending, pending, closing, assigning, failed;
    int32_t user, next_user;
    uint64_t session, sequence, submitted;
} Slot;
static struct {
    host_t *host;
    uc_bt_factory factory;
    char database[256];
    Slot slots[HOST_MAX_PADS];
    int32_t user;
    int selected, stopping, opening, system_assignment;
    uint64_t now;
    const char *error;
} bt = {.selected = -1, .user = -1};
static int valid_slot(int slot)
{
    return slot >= 0 && slot < HOST_MAX_PADS;
}
static unsigned recording_level = 3;
static int rumble_enabled = 1;
void uc_bt_set_rumble_enabled(int enabled)
{
    rumble_enabled = enabled != 0;
    if (!rumble_enabled) {
        for (int i = 0; i < HOST_MAX_PADS; i++) {
            bt.slots[i].notice_pending = 0;
            if (bt.host && bt.slots[i].connected)
                (void)host_rumble_stop(bt.host, i);
        }
    }
}
void uc_bt_set_log_level(unsigned level)
{
    if (level <= 5) {
        recording_level = level;
        uc_log_set_level(level);
    }
}
static void log_code(const char *code)
{
    uc_log_info("[bluetooth] %s", code);
}
static int own_output(const Slot *s)
{
    return s->session && !s->pending && uc_core_session_active(s->session);
}
static int cleanup_output(Slot *s)
{
    if (s->session) {
        int rc = uc_core_disable_session(s->session);
        if (rc && rc != -2) {
            s->closing = 1;
            return 2;
        }
    }
    s->pending = 0;
    s->closing = 0;
    s->session = 0;
    s->sequence = 0;
    s->submitted = 0;
    return 0;
}
static int any_output(void)
{
    for (int i = 0; i < HOST_MAX_PADS; i++)
        if (bt.slots[i].session)
            return 1;
    return 0;
}
static int16_t axis(uint8_t value)
{
    return value >= 128 ? (int16_t)(((int)value - 128) * 32767 / 127)
                        : (int16_t)(((int)value - 128) * 256);
}
static int submit_latest(int slot)
{
    Slot *s = &bt.slots[slot];
    if (!own_output(s) || !s->valid || s->frames == s->submitted || bt.now < s->last_input ||
        bt.now - s->last_input >= 500)
        return 0;
    if (s->sequence == UINT64_MAX) {
        s->closing = 1;
        bt.error = "sequence_exhausted";
        return -1;
    }
    const pad_state *p = &s->latest;
    int16_t axes[4] = {axis(p->lx), axis(p->ly), axis(p->rx), axis(p->ry)};
    uint16_t triggers[2] = {(uint16_t)(p->l2 * 257u), (uint16_t)(p->r2 * 257u)};
    if (uc_core_submit(s->session, ++s->sequence,
                       uc_device_options_buttons(p->buttons, s->options.swap_ab), axes, triggers,
                       bt.now)) {
        s->closing = 1;
        bt.error = "input_submit_failed";
        uc_log_error("[bluetooth] %s", bt.error);
        return -1;
    }
    s->submitted = s->frames;
    return 0;
}
static void on_connect(void *unused, int slot, const pad_info *info)
{
    (void)unused;
    if (!valid_slot(slot) || !info || bt.stopping)
        return;
    Slot *s = &bt.slots[slot];
    if (s->session && cleanup_output(s)) {
        bt.error = "slot_cleanup_pending";
        return;
    }
    if (bt.selected == slot)
        bt.selected = -1;
    memset(s, 0, sizeof(*s));
    s->user = -1;
    s->connected = 1;
    s->connected_at = bt.now;
    s->notice_pending = rumble_enabled;
    s->transport = info->transport == PAD_TRANSPORT_BLE ? PAD_TRANSPORT_BLE : PAD_TRANSPORT_CLASSIC;
    s->vid = info->vid;
    s->pid = info->pid;
    memcpy(s->addr, info->addr, 6);
    snprintf(s->profile, sizeof(s->profile), "%s", info->profile ? info->profile : "unknown");
    snprintf(s->options_key, sizeof s->options_key, "%s:%02X:%02X:%02X:%02X:%02X:%02X:%04X:%04X",
             s->transport == PAD_TRANSPORT_BLE ? "ble" : "classic", s->addr[5], s->addr[4],
             s->addr[3], s->addr[2], s->addr[1], s->addr[0], s->vid, s->pid);
    s->known = profile_device_known(s->transport, s->vid, s->pid);
    s->options = uc_device_options_get(s->options_key, s->known);
    s->notice_pending = rumble_enabled && s->options.rumble;
    uc_log_info("[bluetooth] device options key=%s known=%d rumble=%d swap_ab=%d", s->options_key,
                s->known, s->options.rumble, s->options.swap_ab);
    log_code("physical_connected");
}
static void on_state(void *unused, int slot, const pad_state *state)
{
    (void)unused;
    if (!valid_slot(slot) || !state || !bt.slots[slot].connected || bt.stopping)
        return;
    Slot *s = &bt.slots[slot];
    if (s->frames == UINT64_MAX) {
        s->closing = 1;
        bt.error = "input_counter_exhausted";
        return;
    }
    s->latest = *state;
    s->last_input = bt.now;
    s->valid = 1;
    ++s->frames;
    if (!s->failed && bt.system_assignment && bt.selected == -1 && s->user < 0) {
        bt.selected = slot;
        s->user = 0;
        log_code("system_choice_source_selected");
    } else if (!s->failed && !bt.system_assignment && bt.selected == -1 && s->user <= 0 &&
               uc_backend_can_start(bt.user)) {
        int taken = 0;
        for (int i = 0; i < HOST_MAX_PADS; i++)
            if (bt.slots[i].user == bt.user)
                taken = 1;
        if (!taken) {
            bt.selected = slot;
            s->user = bt.user;
            log_code("first_valid_input");
            if (recording_level >= 3)
                uc_log_info("[bluetooth] binding saved user=%d slot=%d after fresh input", s->user,
                            slot);
        }
    }
    if (own_output(s) &&
        (bt.system_assignment ? uc_backend_transport_ready() : uc_backend_can_start(s->user)))
        (void)submit_latest(slot);
}
static void on_disconnect(void *unused, int slot)
{
    (void)unused;
    if (!valid_slot(slot))
        return;
    bt.slots[slot].connected = 0;
    bt.slots[slot].valid = 0;
    bt.slots[slot].notice_pending = 0;
    Slot *s = &bt.slots[slot];
    if (cleanup_output(s))
        bt.error = "disconnect_cleanup_pending";
    else {
        s->user = -1;
        if (bt.selected == slot)
            bt.selected = -1;
    }
    log_code("physical_disconnected");
}
static host_t *default_open(const char *path, const host_events *events, void *unused)
{
    (void)unused;
#ifdef UC_PS5
    hci_t hci;
    memset(&hci, 0, sizeof(hci));
    if (!hci_usb_open(&hci))
        return NULL;
    host_t *host = host_open(hci, path, events);
    if (!host && hci.ops && hci.ops->close)
        hci.ops->close(hci.ctx);
    return host;
#else
    (void)path;
    (void)events;
    return NULL;
#endif
}
int uc_bt_set_factory(const uc_bt_factory *factory, const char *path)
{
    if (bt.host || bt.opening || bt.stopping || any_output())
        return 2;
    if (path && (!*path || strlen(path) >= sizeof(bt.database)))
        return -1;
    if (factory && !factory->open)
        return -1;
    bt.factory = factory ? *factory : (uc_bt_factory){0};
    snprintf(bt.database, sizeof(bt.database), "%s", path ? path : "/data/unicontroller/bonds.db");
    return 0;
}
#ifdef UC_PS5
static void startup_idle(void *unused)
{
    (void)unused;
    /* Keep power observation live during bounded HCI initialization waits. */
    uc_backend_poll();
    /* Normal host_poll callbacks use the caller's captured bt.now. Advancing
     * core's clock here would make that same poll's input appear regressed. */
    if (bt.opening || bt.stopping) {
        long current = now_ms();
        if (current >= 0)
            (void)uc_core_tick((uint64_t)current);
    }
}
#endif
static int start_transport(int32_t user, int system_assignment)
{
    if (bt.host || bt.opening || bt.stopping || any_output())
        return 2;
    if (system_assignment ? (user < 0 || !uc_backend_transport_ready())
                          : (user <= 0 || !uc_backend_can_start(user))) {
        bt.error = "start_not_ready";
        return -1;
    }
    if (!bt.database[0])
        snprintf(bt.database, sizeof(bt.database), "/data/unicontroller/bonds.db");
    memset(bt.slots, 0, sizeof(bt.slots));
    bt.selected = -1;
    bt.user = user;
    bt.system_assignment = system_assignment;
    bt.error = NULL;
    bt.opening = 1;
    host_events events = {.on_connect = on_connect,
                          .on_state = on_state,
                          .on_disconnect = on_disconnect
#ifdef UC_PS5
                          ,
                          .idle = startup_idle
#endif
    };
    host_t *(*open_host)(const char *, const host_events *, void *) =
        bt.factory.open ? bt.factory.open : default_open;
    bt.host = open_host(bt.database, &events, bt.factory.userdata);
    bt.opening = 0;
    if (!bt.host) {
        bt.user = -1;
        bt.error = "transport_open_failed_or_unsupported";
        uc_log_error("[bluetooth] %s", bt.error);
        return -1;
    }
    log_code(bt.system_assignment ? "started_system_choice_experiment" : "started");
    return 0;
}
int uc_bt_start(int32_t user)
{
    return start_transport(user, 0);
}
int uc_bt_start_system_assigned(void)
{
    return start_transport(0, 1);
}
int uc_bt_stop(void)
{
    bt.stopping = 1;
    int cleanup_pending = 0;
    for (int i = 0; i < HOST_MAX_PADS; i++)
        if (cleanup_output(&bt.slots[i]))
            cleanup_pending = 1;
    /* Close shared HCI even if virtual-device deletion needs later retries. */
    host_t *host = bt.host;
    bt.host = NULL;
    if (host)
        host_close(host);
    if (cleanup_pending)
        return 2;
    memset(bt.slots, 0, sizeof(bt.slots));
    bt.selected = -1;
    bt.user = -1;
    bt.stopping = 0;
    log_code("stopped");
    return 0;
}
void uc_bt_suspend(void)
{
    bt.error = "suspended_manual_restart_required";
    (void)uc_bt_stop();
}
int uc_bt_running(void)
{
    return bt.host != NULL || bt.opening || bt.stopping;
}
/* One bounded pulse per physical connection; never delay input processing. */
static void poll_connection_notice(uint64_t now)
{
    for (int slot = 0; slot < HOST_MAX_PADS; slot++) {
        Slot *s = &bt.slots[slot];
        if (!s->connected || !s->notice_pending)
            continue;
        if (now < s->connected_at || !rumble_enabled || !s->options.rumble) {
            s->notice_pending = 0;
            continue;
        }
        uint64_t elapsed = now - s->connected_at;
        if (elapsed < 1000)
            continue;
        /* Late resumes must not deliver a stale connection pulse. */
        if (elapsed > 5000) {
            s->notice_pending = 0;
            log_code("connection_rumble_expired");
            continue;
        }
        if (!host_output_ready(bt.host, slot)) {
            if (elapsed == 5000) {
                s->notice_pending = 0;
                log_code("connection_rumble_unavailable");
            }
            continue;
        }
        s->notice_pending = 0;
        int accepted = host_rumble_pulse(bt.host, slot, 200);
        if (recording_level >= 3)
            uc_log_info("[bluetooth] connection_rumble slot=%d duration_ms=200 accepted=%d", slot,
                        accepted != 0);
    }
}
void uc_bt_poll(uint64_t now)
{
    bt.now = now;
    if (bt.stopping) {
        (void)uc_bt_stop();
        return;
    }
    if (!bt.host)
        return;
    int working_user = uc_backend_can_start(bt.user);
    for (int i = 0; i < HOST_MAX_PADS; i++)
        if (bt.slots[i].user > 0 && uc_backend_can_start(bt.slots[i].user))
            working_user = 1;
    if (bt.system_assignment ? !uc_backend_transport_ready() : !working_user) {
        bt.error = "power_or_user_unavailable";
        (void)uc_bt_stop();
        return;
    }
    host_poll(bt.host, 0);
    if (host_transport_lost(bt.host)) {
        bt.error = "transport_lost_manual_restart_required";
        (void)uc_bt_stop();
        return;
    }
    if (bt.stopping) {
        (void)uc_bt_stop();
        return;
    }
    poll_connection_notice(now);
    for (int slot = 0; slot < HOST_MAX_PADS; slot++) {
        Slot *s = &bt.slots[slot];
        if (s->closing || (!s->connected && s->session)) {
            if (!cleanup_output(s)) {
                if (s->assigning && s->connected) {
                    s->user = s->next_user;
                    s->assigning = 0;
                    s->valid = 0;
                } else {
                    s->user = -1;
                    s->assigning = 0;
                    if (bt.selected == slot)
                        bt.selected = -1;
                }
            }
            continue;
        }
        if (s->failed || !s->connected || s->user < 0 || (s->user == 0 && !bt.system_assignment))
            continue;
        if (bt.system_assignment ? !uc_backend_transport_ready() : !uc_backend_can_start(s->user)) {
            s->valid = 0;
            if (cleanup_output(s))
                s->closing = 1;
            else {
                s->user = -1;
                if (bt.selected == slot)
                    bt.selected = -1;
            }
            bt.error = "assigned_user_unavailable";
            continue;
        }
        if (!s->valid || now < s->last_input) {
            if (s->pending && cleanup_output(s))
                s->closing = 1;
            continue;
        }
        if (!s->session || s->pending) {
            if (!s->session && now - s->last_input >= 500)
                continue;
            uint64_t session = s->session;
            int rc = bt.system_assignment ? uc_core_enable_system_assigned(now, &session)
                                          : uc_core_enable_physical(s->user, now, &session);
            if (session)
                s->session = session;
            if (rc == 2) {
                s->pending = session != 0;
                if (session)
                    s->session = session;
                continue;
            }
            if (rc || !session) {
                bt.error = "virtual_enable_failed_manual_retry_required";
                s->failed = 1;
                s->closing = 1;
                if (recording_level >= 1)
                    uc_log_error(
                        "[bluetooth] virtual_create_failed slot=%d user=%d session=%llu rc=%d; "
                        "automatic retry disabled",
                        slot, s->user, (unsigned long long)s->session, rc);
                /* A failed virtual output must not keep competing with the
                 * console for shared HCI packets. Stop all owned links and
                 * release HCI even when virtual cleanup needs later polls. */
                (void)uc_bt_stop();
                return;
            }
            s->pending = 0;
            s->session = session;
            s->sequence = 0;
            s->submitted = 0;
            log_code(bt.system_assignment ? "virtual_system_choice_ready_binding_unverified"
                                          : "virtual_bound");
        } else if (!own_output(s)) {
            bt.error = "session_lost_manual_assignment_required";
            s->failed = 1;
            s->closing = 1;
            continue;
        }
        (void)submit_latest(slot);
        if (rumble_enabled && s->options.rumble && own_output(s)) {
            uint8_t strong = 0, weak = 0;
            int sample = uc_core_read_rumble(s->session, now, &strong, &weak);
            if (sample == 1)
                (void)host_rumble_feedback(bt.host, slot, strong, weak);
        }
    }
}

int uc_bt_scan(unsigned seconds)
{
    if (!bt.host || bt.stopping || seconds > 120)
        return -1;
    return host_scan(bt.host, (int)seconds) ? 0 : -1;
}
int uc_bt_pair(uint32_t discovery_id)
{
    if (!bt.host || bt.stopping || !discovery_id)
        return -1;
    return host_pair_device(bt.host, discovery_id) ? 0 : -1;
}
int uc_bt_forget(const char *address)
{
    if (!bt.host || bt.stopping || !address || strlen(address) != 17)
        return -1;
    unsigned char bytes[6];
    for (int i = 0; i < 6; i++) {
        unsigned value = 0;
        for (int n = 0; n < 2; n++) {
            unsigned char c = (unsigned char)address[i * 3 + n];
            unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                    : 16;
            if (digit > 15)
                return -1;
            value = value * 16 + digit;
        }
        if (i < 5 && address[i * 3 + 2] != ':')
            return -1;
        bytes[5 - i] = (unsigned char)value;
    }
    return host_forget(bt.host, bytes) ? 0 : -1;
}
static int append(char *b, size_t cap, size_t *used, const char *format, ...)
{
    if (*used >= cap)
        return -1;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(b + *used, cap - *used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}
static int json_text(char *buffer, size_t capacity, size_t *used, const char *text)
{
    if (append(buffer, capacity, used, "\""))
        return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        int result = *p == '"' || *p == '\\' ? append(buffer, capacity, used, "\\%c", *p)
                     : *p < 32               ? append(buffer, capacity, used, "\\u%04x", *p)
                                             : append(buffer, capacity, used, "%c", *p);
        if (result)
            return -1;
    }
    return append(buffer, capacity, used, "\"");
}
static void address_text(const unsigned char a[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}
int uc_bt_assign(int slot, const char *address, int32_t user)
{
    if (bt.system_assignment || !bt.host || bt.stopping || !valid_slot(slot) || !address ||
        user <= 0)
        return -1;
    Slot *s = &bt.slots[slot];
    char current[18];
    address_text(s->addr, current);
    if (!s->connected || strcmp(address, current))
        return -1;
    if (s->assigning)
        return s->next_user == user ? 2 : -1;
    if (s->user == user && !s->closing && !s->failed)
        return 0;
    if (!uc_backend_can_start(user))
        return -1;
    for (int i = 0; i < HOST_MAX_PADS; i++)
        if (i != slot &&
            (bt.slots[i].user == user || (bt.slots[i].assigning && bt.slots[i].next_user == user)))
            return -1;
    s->failed = 0; /* Explicit assignment permits one new creation attempt. */
    s->notice_pending = 0;
    (void)host_rumble_stop(bt.host, slot);
    if (cleanup_output(s)) {
        s->next_user = user;
        s->assigning = 1;
        return 2;
    }
    s->user = user;
    /* Require a new physical report after changing ownership. */
    s->valid = 0;
    return 0;
}
static void safe_profile(const char *input, char out[64])
{
    size_t at = 0;
    for (size_t i = 0; input[i] && at < 63; i++) {
        unsigned char c = (unsigned char)input[i];
        out[at++] = c >= 32 && c < 127 && c != '"' && c != '\\' ? (char)c : '_';
    }
    out[at] = 0;
}
size_t uc_bt_status_json(char *buffer, size_t capacity)
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
    const Slot *summary = valid_slot(bt.selected) ? &bt.slots[bt.selected] : NULL;
    if (!summary)
        for (int i = 0; i < HOST_MAX_PADS; i++)
            if (bt.slots[i].session) {
                summary = &bt.slots[i];
                break;
            }
    int pending = 0;
    for (int i = 0; i < HOST_MAX_PADS; i++)
        pending |= bt.slots[i].pending || bt.slots[i].assigning;
    const char *state = bt.stopping || bt.opening ? "starting"
                        : bt.host                 ? "on"
                        : bt.error                ? "failed"
                                                  : "off";
    ADD("{\"state\":\"%s\",\"reason\":\"%s\",\"running\":%s,\"stopping\":%s,\"user_id\":%d,"
        "\"selected_slot\":%d,\"bound\":%s,\"session_id\":%llu,\"pending\":%s,\"scan_seconds\":%"
        "d,\"error\":",
        state, bt.error ? bt.error : "", uc_bt_running() ? "true" : "false",
        bt.stopping ? "true" : "false", bt.user, bt.selected,
        summary && summary->user > 0 && own_output(summary) ? "true" : "false",
        (unsigned long long)(summary ? summary->session : 0), pending ? "true" : "false",
        bt.host ? host_scan_left(bt.host) : 0);
    if (bt.error)
        ADD("\"%s\"", bt.error);
    else
        ADD("null");
    ADD(",\"scan_id\":%u", bt.host ? host_scan_id(bt.host) : 0);
    ADD(",\"system_assignment\":%s,\"output_active\":%s",
        bt.system_assignment && bt.host ? "true" : "false",
        summary && own_output(summary) ? "true" : "false");
    ADD(",\"devices\":[");
    int count = 0;
    for (int i = 0; i < HOST_MAX_PADS; i++) {
        Slot *s = &bt.slots[i];
        if (!s->connected)
            continue;
        char addr[18], profile[64], model[64];
        address_text(s->addr, addr);
        safe_profile(s->profile, profile);
        safe_profile(pad_display_name_for_transport(s->transport, s->vid, s->pid, profile), model);
        ADD("%s{\"slot\":%d,\"address\":\"%s\",\"vid\":\"%04X\",\"pid\":\"%04X\",\"profile\":\"%"
            "s\",\"model\":\"%s\",\"connected\":true,\"bound\":%s,\"user_id\":%d,\"session_id\":%"
            "llu,\"pending\":%s,\"input_frames\":%llu,\"last_"
            "input_ms\":%llu,"
            "\"battery\":",
            count++ ? "," : "", i, addr, s->vid, s->pid, profile, model,
            s->user > 0 && own_output(s) ? "true" : "false", s->user,
            (unsigned long long)s->session, (s->pending || s->assigning) ? "true" : "false",
            (unsigned long long)s->frames, (unsigned long long)s->last_input);
        if (s->valid && s->latest.has_battery)
            ADD("%u", s->latest.battery_pct <= 100 ? s->latest.battery_pct : 100);
        else
            ADD("-1");
        ADD(",\"system_assignment\":%s,\"output_active\":%s",
            bt.system_assignment ? "true" : "false", own_output(s) ? "true" : "false");
        ADD(",\"output_failed\":%s", s->failed ? "true" : "false");
        ADD(",\"settings_key\":\"%s\",\"rumble\":%s,\"swap_ab\":%s,\"known_device\":%s,\"rumble_"
            "available\":%s,\"rumble_enabled\":%s",
            s->options_key, s->options.rumble ? "true" : "false",
            s->options.swap_ab ? "true" : "false", s->known ? "true" : "false",
            host_output_ready(bt.host, i) ? "true" : "false",
            rumble_enabled && s->options.rumble ? "true" : "false");
        ADD(",\"charging\":%s}", s->valid && s->latest.charging ? "true" : "false");
    }
    ADD("],\"discovered\":[");
    int found_count = bt.host ? host_discovered_count(bt.host) : 0;
    for (int i = 0; i < found_count; ++i) {
        host_discovered_info found;
        if (!host_discovered_get(bt.host, i, &found))
            continue;
        char address[18];
        address_text(found.addr, address);
        ADD("%s{\"id\":%u,\"address\":\"%s\",\"transport\":\"%s\",\"address_type\":%d,\"state\":\"%"
            "s\",\"rssi\":",
            i ? "," : "", found.id, address, found.le ? "ble" : "classic", found.addr_type,
            found.state);
        if (found.has_rssi)
            ADD("%d", found.rssi);
        else
            ADD("null");
        ADD(",\"name\":");
        if (json_text(buffer, capacity, &used, found.name))
            return 0;
        ADD(",\"reason\":");
        if (json_text(buffer, capacity, &used, found.reason))
            return 0;
        ADD("}");
    }
    ADD("],\"paired\":[");
    count = 0;
    int paired = bt.host ? host_paired_count(bt.host) : 0;
    for (int i = 0; i < paired && i < 64; i++) {
        host_paired_info p;
        if (!host_paired_get(bt.host, i, &p))
            continue;
        char addr[18];
        address_text(p.addr, addr);
        ADD("%s{\"address\":\"%s\",\"vid\":\"%04X\",\"pid\":\"%04X\",\"le\":%s,\"connected\":%s}",
            count++ ? "," : "", addr, p.vid, p.pid, p.le ? "true" : "false",
            p.connected ? "true" : "false");
    }
    ADD("]}");
#undef ADD
    return used;
}

#define ADD(...)                                                                                   \
    do {                                                                                           \
        if (append(buffer, capacity, &used, __VA_ARGS__)) {                                        \
            buffer[0] = 0;                                                                         \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)
/* Snapshot only: no transport commands, feature reads, or output probes. */
size_t uc_bt_diagnostics_json(char *buffer, size_t capacity)
{
    if (!buffer || !capacity)
        return 0;
    size_t used = 0;
    buffer[0] = 0;
    ADD("{\"schema_version\":1,\"snapshot\":true,\"devices\":[");
    int count = 0;
    for (int slot = 0; slot < HOST_MAX_PADS; slot++) {
        const Slot *s = &bt.slots[slot];
        if (!s->connected)
            continue;
        char address[18], profile[64];
        address_text(s->addr, address);
        safe_profile(s->profile, profile);
        host_hid_descriptor descriptor = {0};
        int available = host_hid_descriptor_get(bt.host, slot, &descriptor);
        ADD("%s{\"slot\":%d,\"address\":\"%s\",\"transport\":\"%s\","
            "\"vid\":\"%04X\",\"pid\":\"%04X\",\"profile\":\"%s\","
            "\"rumble_output_ready\":%s,\"descriptor_available\":%s",
            count++ ? "," : "", slot, address,
            s->transport == PAD_TRANSPORT_BLE ? "ble" : "classic", s->vid, s->pid, profile,
            host_output_ready(bt.host, slot) ? "true" : "false", available ? "true" : "false");
        if (available) {
            hid_output_inventory inventory;
            generic_output_inventory(descriptor.data, descriptor.length, &inventory);
            ADD(",\"descriptor_source\":\"%s\",\"descriptor_length\":%d,"
                "\"descriptor_truncated\":%s,\"descriptor_hex\":\"",
                s->transport == PAD_TRANSPORT_BLE ? "gatt_report_map" : "sdp_hid_descriptor",
                descriptor.length, descriptor.truncated ? "true" : "false");
            for (int i = 0; i < descriptor.length; i++)
                ADD("%02x", descriptor.data[i]);
            ADD("\",\"inventory_valid\":%s,\"inventory_truncated\":%s,"
                "\"pid_usage\":%s,\"vendor_usage\":%s,\"reports\":[",
                inventory.valid ? "true" : "false", inventory.truncated ? "true" : "false",
                inventory.pid_usage ? "true" : "false", inventory.vendor_usage ? "true" : "false");
            for (unsigned i = 0; i < inventory.count; i++) {
                const hid_report_inventory *r = &inventory.reports[i];
                ADD("%s{\"type\":\"%s\",\"report_id\":%u,\"bits\":%u,"
                    "\"payload_bytes\":%u,\"report_id_prefix\":%s,\"usage_pages\":[",
                    i ? "," : "", r->type == 2 ? "output" : "feature", r->id, r->bits,
                    (r->bits + 7) / 8, inventory.has_ids ? "true" : "false");
                for (unsigned j = 0; j < r->page_count; j++)
                    ADD("%s%u", j ? "," : "", r->pages[j]);
                ADD("]}");
            }
            ADD("]");
        }
        ADD("}");
    }
    ADD("]}");
    return used;
}

#undef ADD

int uc_bt_device_options_match(const char *key, uint64_t session)
{
    for (int i = 0; i < HOST_MAX_PADS; ++i) {
        Slot *s = &bt.slots[i];
        if (s->connected && s->session == session && !strcmp(s->options_key, key))
            return 1;
    }
    return 0;
}
void uc_bt_device_options_changed(void)
{
    for (int i = 0; i < HOST_MAX_PADS; ++i) {
        Slot *s = &bt.slots[i];
        if (!s->connected)
            continue;
        uc_device_options next = uc_device_options_get(s->options_key, s->known);
        if (s->options.rumble && !next.rumble) {
            s->notice_pending = 0;
            if (bt.host)
                (void)host_rumble_stop(bt.host, i);
        }
        if (s->options.swap_ab != next.swap_ab)
            s->submitted = 0;
        s->options = next;
    }
}

int uc_bt_test_rumble(const char *key, uint64_t session, uint64_t now)
{
    if (!key || !bt.host || bt.stopping || !rumble_enabled)
        return -1;
    for (int i = 0; i < HOST_MAX_PADS; ++i) {
        Slot *s = &bt.slots[i];
        if (!s->connected || s->session != session || strcmp(s->options_key, key))
            continue;
        if (!s->options.rumble || !host_output_ready(bt.host, i))
            return -1;
        if (s->rumble_test_seen && (now < s->rumble_test_last || now - s->rumble_test_last < 1000))
            return 2;
        int accepted = host_rumble_pulse(bt.host, i, 500);
        uc_log_info("[bluetooth] rumble test key=%s duration_ms=500 accepted=%d", key,
                    accepted != 0);
        if (!accepted)
            return -1;
        s->notice_pending = 0;
        s->rumble_test_seen = 1;
        s->rumble_test_last = now;
        return 0;
    }
    return -1;
}
