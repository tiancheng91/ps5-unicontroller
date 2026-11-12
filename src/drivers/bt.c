/* One physical source, one explicitly selected PS5 user, no synthetic input heartbeat. */
#include "bt.h"
#include "common/log.h"
#include "protocols/profiles.h"
#include "../core/uc_core.h"
#include "../ps5/uc_backend.h"
#ifdef UC_PS5
#include "bluetooth/hci_usb.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

/* Boolean gate: signed-in user, stable working power and no foreign output. */
extern int uc_backend_can_start(int32_t user);
typedef struct {
    int connected, valid;
    pad_transport transport;
    uint16_t vid, pid;
    unsigned char addr[6];
    char profile[64];
    pad_state latest;
    uint64_t last_input, frames, connected_at;
    int notice_pending;
} Slot;
static struct {
    host_t *host;
    uc_bt_factory factory;
    char database[256];
    Slot slots[HOST_MAX_PADS];
    int32_t user;
    int selected, stopping, pending, opening;
    uint64_t now, session, sequence, submitted;
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
        for (int i = 0; i < HOST_MAX_PADS; i++)
            bt.slots[i].notice_pending = 0;
    }
}
void uc_bt_set_log_level(unsigned level)
{
    if (level <= 5) {
        recording_level = level;
        log_set_level((int)level);
    }
}
static void log_code(const char *code)
{
    if (recording_level >= 3)
        fprintf(stderr, "[bluetooth] %s\n", code);
}
static int own_output(void)
{
    return bt.session && uc_core_active_session() == bt.session;
}
static int cleanup_output(void)
{
    if (bt.pending || own_output()) {
        if (uc_core_disable())
            return 2;
    }
    bt.pending = 0;
    bt.session = 0;
    bt.sequence = 0;
    bt.submitted = 0;
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
    if (!own_output() || !s->valid || s->frames == bt.submitted || bt.now < s->last_input ||
        bt.now - s->last_input > 500)
        return 0;
    if (bt.sequence == UINT64_MAX) {
        bt.stopping = 1;
        bt.error = "sequence_exhausted";
        return -1;
    }
    const pad_state *p = &s->latest;
    int16_t axes[4] = {axis(p->lx), axis(p->ly), axis(p->rx), axis(p->ry)};
    uint16_t triggers[2] = {(uint16_t)(p->l2 * 257u), (uint16_t)(p->r2 * 257u)};
    if (uc_core_submit(bt.session, ++bt.sequence, p->buttons, axes, triggers, bt.now)) {
        bt.stopping = 1;
        bt.error = "input_submit_failed";
        log_code(bt.error);
        return -1;
    }
    bt.submitted = s->frames;
    return 0;
}
static void on_connect(void *unused, int slot, const pad_info *info)
{
    (void)unused;
    if (!valid_slot(slot) || !info || bt.stopping)
        return;
    Slot *s = &bt.slots[slot];
    memset(s, 0, sizeof(*s));
    s->connected = 1;
    s->connected_at = bt.now;
    s->notice_pending = rumble_enabled;
    s->transport = info->transport == PAD_TRANSPORT_BLE ? PAD_TRANSPORT_BLE : PAD_TRANSPORT_CLASSIC;
    s->vid = info->vid;
    s->pid = info->pid;
    memcpy(s->addr, info->addr, 6);
    snprintf(s->profile, sizeof(s->profile), "%s", info->profile ? info->profile : "unknown");
    log_code("physical_connected");
}
static void on_state(void *unused, int slot, const pad_state *state)
{
    (void)unused;
    if (!valid_slot(slot) || !state || !bt.slots[slot].connected || bt.stopping)
        return;
    Slot *s = &bt.slots[slot];
    if (s->frames == UINT64_MAX) {
        bt.stopping = 1;
        bt.error = "input_counter_exhausted";
        return;
    }
    s->latest = *state;
    s->last_input = bt.now;
    s->valid = 1;
    ++s->frames;
    if (bt.selected == -1 && !bt.session && !bt.pending) {
        bt.selected = slot;
        log_code("first_valid_input");
    }
    if (bt.selected == slot && own_output())
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
    if (bt.selected == slot) {
        /* Selection stays occupied until cleanup completes; another source must send a new frame.
         */
        if (cleanup_output()) {
            bt.stopping = 1;
            bt.error = "disconnect_cleanup_pending";
        } else
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
    if (bt.host || bt.opening || bt.stopping || bt.session || bt.pending)
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
}
#endif
int uc_bt_start(int32_t user)
{
    if (bt.host || bt.opening || bt.stopping || bt.session || bt.pending ||
        uc_core_active_session())
        return 2;
    if (user <= 0 || !uc_backend_can_start(user)) {
        bt.error = "start_not_ready";
        return -1;
    }
    if (!bt.database[0])
        snprintf(bt.database, sizeof(bt.database), "/data/unicontroller/bonds.db");
    memset(bt.slots, 0, sizeof(bt.slots));
    bt.selected = -1;
    bt.user = user;
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
        log_code(bt.error);
        return -1;
    }
    log_code("started");
    return 0;
}
int uc_bt_stop(void)
{
    bt.stopping = 1;
    int cleanup_pending = cleanup_output();
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
        if (now < s->connected_at || !rumble_enabled) {
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
            fprintf(stderr, "[bluetooth] connection_rumble slot=%d duration_ms=200 accepted=%d\n",
                    slot, accepted != 0);
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
    if (!uc_backend_can_start(bt.user)) {
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
    if (bt.selected < 0)
        return;
    Slot *s = &bt.slots[bt.selected];
    if (!s->connected || !s->valid || now < s->last_input) {
        /* Disconnect/corrupt time cancels creation. A slow identification may finish
         * neutral; submit_latest separately refuses stale held-button data. */
        if (bt.pending) {
            if (cleanup_output())
                bt.stopping = 1;
        }
        return;
    }
    if (!bt.session) {
        if (uc_core_active_session()) {
            bt.error = "foreign_output_busy";
            (void)uc_bt_stop();
            return;
        }
        uint64_t session = 0;
        int rc = uc_core_enable(bt.user, now, &session);
        if (rc == 2) {
            bt.pending = 1;
            return;
        }
        if (rc || !session) {
            bt.error = "virtual_enable_failed";
            (void)uc_bt_stop();
            return;
        }
        bt.pending = 0;
        bt.session = session;
        bt.sequence = 0;
        bt.submitted = 0;
        log_code("virtual_bound");
    } else if (!own_output()) {
        bt.error = "session_lost_manual_restart_required";
        (void)uc_bt_stop();
        return;
    }
    (void)submit_latest(bt.selected);
}
int uc_bt_pair(unsigned seconds)
{
    if (!bt.host || bt.stopping || seconds == 0 || seconds > 120)
        return -1;
    return host_pair(bt.host, (int)seconds) ? 0 : -1;
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
static void address_text(const unsigned char a[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
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
    const char *state = bt.stopping || bt.opening ? "starting"
                        : bt.host                 ? "on"
                        : bt.error                ? "failed"
                                                  : "off";
    ADD("{\"state\":\"%s\",\"reason\":\"%s\",\"running\":%s,\"stopping\":%s,\"user_id\":%d,"
        "\"selected_slot\":%d,\"bound\":%s,\"session_id\":%llu,\"pending\":%s,\"pairing_seconds\":%"
        "d,\"error\":",
        state, bt.error ? bt.error : "", uc_bt_running() ? "true" : "false",
        bt.stopping ? "true" : "false", bt.user, bt.selected, own_output() ? "true" : "false",
        (unsigned long long)bt.session, bt.pending ? "true" : "false",
        bt.host ? host_pairing_left(bt.host) : 0);
    if (bt.error)
        ADD("\"%s\"", bt.error);
    else
        ADD("null");
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
            "s\",\"model\":\"%s\",\"connected\":true,\"bound\":%s,\"input_frames\":%llu,\"last_"
            "input_ms\":%llu,"
            "\"battery\":",
            count++ ? "," : "", i, addr, s->vid, s->pid, profile, model,
            bt.selected == i && own_output() ? "true" : "false", (unsigned long long)s->frames,
            (unsigned long long)s->last_input);
        if (s->valid && s->latest.has_battery)
            ADD("%u", s->latest.battery_pct <= 100 ? s->latest.battery_pct : 100);
        else
            ADD("-1");
        ADD(",\"charging\":%s}", s->valid && s->latest.charging ? "true" : "false");
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
