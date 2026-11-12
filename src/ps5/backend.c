/* Independent narrow PS5 virtual-pad adapter. No physical-controller I/O/hooks. */
#include "uc_backend.h"
#include "../core/bt_autostart.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint16_t x, y;
    uint8_t id, reserved[3];
} Touch;
typedef struct {
    uint8_t count, reserved[3];
    uint32_t unused;
    Touch touches[2];
} Touches;
typedef struct {
    uint32_t buttons;
    uint8_t axes[4], triggers[2], padding[2];
    float orientation[4], acceleration[3], angular[3];
    Touches touch;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t extension[16], count, reserved[2], unique_len, unique[12];
} PadData;
_Static_assert(sizeof(PadData) == 120, "pad ABI size");
_Static_assert(offsetof(PadData, timestamp) == 80, "pad timestamp ABI");
int32_t sceUserServiceInitialize(void *);
int32_t sceUserServiceGetLoginUserIdList(int32_t[4]);
int32_t sceUserServiceGetForegroundUser(int32_t *);
int32_t scePadInit(void);
int32_t scePadSetProcessPrivilege(int32_t);
int32_t scePadVirtualDeviceAddDevice(void *, int32_t);
int32_t scePadVirtualDeviceInsertData(int32_t, const void *);
int32_t scePadVirtualDeviceDeleteDevice(int32_t);
uint64_t kernel_get_ucred_authid(pid_t);
int32_t kernel_get_ucred_caps(pid_t, uint8_t[16]);
int32_t kernel_set_ucred_authid(pid_t, uint64_t);
int32_t kernel_set_ucred_caps(pid_t, const uint8_t[16]);
int sceKernelOpenEventFlag(intptr_t *, const char *);
int sceKernelPollEventFlag(intptr_t, uint64_t, unsigned, uint64_t *);
int sceKernelCloseEventFlag(intptr_t);
static int (*bind_user)(uint64_t, int32_t);
typedef int32_t (*remote_setting_fn)(int32_t, void *);
static remote_setting_fn remote_setting;
typedef struct {
    int used, closing, input_logged, ps_logged;
    int32_t owner;
    uint64_t device, token, feedback_at;
    int feedback_polled, feedback_failed;
} owned_output;
static owned_output outputs[4];
static int pending_slot = -1;
static owned_output *find_output(uint64_t handle)
{
    if (!handle)
        return NULL;
    for (unsigned i = 0; i < 4; i++)
        if (outputs[i].used && outputs[i].token == handle)
            return &outputs[i];
    return NULL;
}

static int ready, phase, fd = -1, matches, cancelled, discard_line, connection_pending, identified;
static int raw_log, log_copy = -1;
static size_t consumed;
static int32_t owner = -1;
static uint64_t device, started, matched, last_poll, serial = 1;
static char line[1024];
static size_t used;
static uint64_t old_auth;
static uint8_t old_caps[16];
static int raised;
static intptr_t power_flag = -1;
static int suspend_event, rest_seen, power_status = -1;
static uint64_t working_since;
static uc_bt_autostart home_gate;
static int home_waiting, home_owns_log, auto_mode;
static int32_t home_foreground = -1;
static uint64_t home_connect_at;
static uint64_t system_users_at;
static int system_users_valid;
static int32_t system_users_last[5];
/* At most one error per stage per process, including repeated frame failures. */
static uint32_t reported_errors;
static void backend_error(unsigned stage, const char *operation, int32_t result)
{
    uint32_t mask = UINT32_C(1) << stage;
    if (!(reported_errors & mask)) {
        reported_errors |= mask;
        fprintf(stderr, "UniController backend: %s failed rc=%#x\n", operation, (unsigned)result);
    }
}
static int ambiguity_elapsed(uint64_t now, uint64_t first_match)
{
    return now >= first_match && now - first_match >= 300;
}
static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int credentials(uint64_t auth, const uint8_t caps[16])
{
    uint8_t verify[16];
    pid_t p = getpid();
    return kernel_set_ucred_authid(p, auth) == 0 && kernel_set_ucred_caps(p, caps) == 0 &&
           kernel_get_ucred_authid(p) == auth && kernel_get_ucred_caps(p, verify) == 0 &&
           memcmp(caps, verify, 16) == 0;
}
static void close_log(void)
{
    if (log_copy >= 0)
        close(log_copy);
    log_copy = -1;
    raw_log = 0;
    if (fd >= 0)
        close(fd);
    fd = -1;
    used = 0;
    discard_line = 0;
    connection_pending = 0;
}
int uc_backend_users(int32_t users[4])
{
    if (!users)
        return -1;
    for (int i = 0; i < 4; i++)
        users[i] = -1;
    return sceUserServiceGetLoginUserIdList(users) == 0 ? 0 : -1;
}
static int user_live(int32_t user)
{
    int32_t ids[4];
    if (user <= 0 || uc_backend_users(ids))
        return 0;
    for (int i = 0; i < 4; i++)
        if (ids[i] == user)
            return 1;
    return 0;
}
int uc_backend_transport_ready(void)
{
    uint64_t current = now_ms();
    return ready && power_status == 0 && working_since && current >= working_since &&
           current - working_since >= 5000;
}
int uc_backend_can_start(int32_t user)
{
    return user_live(user) && uc_backend_transport_ready();
}
int uc_backend_init(void)
{
    if (ready)
        return 0;
    old_auth = kernel_get_ucred_authid(getpid());
    if (!old_auth || kernel_get_ucred_caps(getpid(), old_caps)) {
        backend_error(0, "read_credentials", -1);
        return -1;
    }
    uint8_t all[16];
    memset(all, 255, sizeof all);
    raised = 1;
    if (!credentials(UINT64_C(0x3800000000010003), all)) {
        backend_error(1, "elevate_credentials", -1);
        goto fail;
    }
    void *mb = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!mb)
        mb = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!mb ||
        !(bind_user = (int (*)(uint64_t, int32_t))dlsym(mb, "sceMbusBindDeviceWithUserId"))) {
        backend_error(2, "load_mbus", -1);
        goto fail;
    }
    (void)sceUserServiceInitialize(NULL);
    int32_t rc = scePadInit();
    if (rc != 0) {
        backend_error(3, "scePadInit", rc);
        goto fail;
    }
    rc = scePadSetProcessPrivilege(1);
    if (rc != 0) {
        backend_error(4, "scePadSetProcessPrivilege", rc);
        goto fail;
    }
    void *pad = dlopen("/system/common/lib/libScePad.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!pad)
        pad = dlopen("libScePad.sprx", RTLD_NOW | RTLD_GLOBAL);
    remote_setting =
        pad ? (remote_setting_fn)dlsym(pad, "scePadVirtualDeviceGetRemoteSetting") : NULL;
    fprintf(stderr, "UniController: game rumble feedback %s\n",
            remote_setting ? "available" : "unavailable");
    ready = 1;
    last_poll = now_ms();
    return 0;
fail:
    if (credentials(old_auth, old_caps))
        raised = 0;
    return -1;
}
static int raw_log_open(void)
{
    close_log();
    (void)mkdir("/data/unicontroller", 0700);
    log_copy =
        open("/data/unicontroller/kernel-identification.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_copy < 0)
        return -1;
    fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        close_log();
        return -1;
    }
    raw_log = 1;
    consumed = 0;
    fprintf(stderr, "UniController: identification source /dev/klog; copying consumed bytes to "
                    "kernel-identification.log\n");
    return 0;
}
static int open_log(void)
{
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return raw_log_open();
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close_log();
        return -1;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(3232);
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0)
        return 0;
    if (errno == EINPROGRESS) {
        connection_pending = 1;
        return 0;
    }
    return raw_log_open();
}
static void candidate(const char *text)
{
    if (!strstr(text, "SCE_MBUS_EVENT_DEVICE_ADDED") || !strstr(text, "[type:1]") ||
        !strstr(text, "[subType:22]") || !strstr(text, "[capabilityBattery:0]"))
        return;
    const char *p = strstr(text, "[DeviceId:0x");
    if (!p)
        return;
    p += 12;
    uint64_t value = 0;
    int digits = 0;
    while (*p && *p != ']') {
        unsigned digit;
        if (*p >= '0' && *p <= '9')
            digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            digit = *p - 'A' + 10;
        else
            return;
        if (++digits > 16)
            return;
        value = value * 16 + digit;
        p++;
    }
    if (*p != ']' || !digits || !value)
        return;
    /* Existing owned devices are never candidates for another creation. Raw
     * ScePad handles use the low word, so reject that collision as well. */
    for (unsigned i = 0; i < 4; i++)
        if (outputs[i].used && (uint32_t)outputs[i].device == (uint32_t)value)
            return;
    if (!matches) {
        device = value;
        matched = now_ms();
        matches = 1;
    } else if (device != value)
        matches = 2;
}
/* Returns 1 only when the source has reached EAGAIN (fresh boundary). */
static int read_log(int identify)
{
    char buffer[1024];
    for (int round = 0; round < 8; round++) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        if (n <= 0) {
            phase = 5;
            close_log();
            return 0;
        }
        if (raw_log) {
            size_t offset = 0;
            while (offset < (size_t)n) {
                ssize_t copied = write(log_copy, buffer + offset, (size_t)n - offset);
                if (copied <= 0) {
                    phase = 5;
                    close_log();
                    return 0;
                }
                offset += (size_t)copied;
            }
            consumed += (size_t)n;
            if (consumed > 256 * 1024) {
                phase = 5;
                close_log();
                return 0;
            }
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buffer[i] == '\n') {
                if (!discard_line) {
                    line[used] = 0;
                    if (home_owns_log) {
                        int previous_home = home_gate.home;
                        uc_bt_auto_line(&home_gate, line);
                        if (previous_home != home_gate.home)
                            fprintf(stderr, "[bluetooth-auto] fresh desktop focus=%d\n",
                                    home_gate.home);
                    }
                    if (identify)
                        candidate(line);
                }
                used = 0;
                discard_line = 0;
            } else if (used < sizeof(line) - 1 && !discard_line)
                line[used++] = buffer[i];
            else
                discard_line = 1;
        }
    }
    return 0;
}
/* Shares the identification reader: never two competing /dev/klog readers.
 * Desktop focus is a compatibility signal, not a physical-pad login callback. */
void uc_backend_auto_watch(int mode)
{
    if (home_owns_log)
        close_log();
    home_owns_log = 0;
    auto_mode = mode;
    home_waiting = mode != UC_AUTO_OFF;
    home_foreground = -1;
    uc_bt_auto_cancel(&home_gate);
    fprintf(stderr, "[bluetooth-auto] mode=%d (%s)\n", mode,
            mode == UC_AUTO_RESUME ? "waiting for stable resume; system user-selection experiment"
            : mode == UC_AUTO_DESKTOP ? "waiting for fresh desktop event"
                                      : "disabled/cancelled");
}
static void poll_home(uint64_t now)
{
    if (!home_waiting && !home_gate.armed)
        return;
    if (suspend_event || power_status != 0) {
        if (home_owns_log)
            close_log();
        home_owns_log = 0;
        home_waiting = 1;
        uc_bt_auto_cancel(&home_gate);
        return;
    }
    /* Wait for any in-flight virtual-device identification to finish. */
    if (phase >= 1 && phase <= 3)
        return;
    if (auto_mode == UC_AUTO_RESUME) {
        if (home_waiting) {
            home_waiting = 0;
            uc_bt_auto_arm(&home_gate, now);
            home_gate.fresh = home_gate.home = 1; /* Power event; no log dependency. */
            fprintf(stderr, "[bluetooth-auto] resume observed; waiting 5000ms; no user required\n");
        }
        return;
    }
    if (home_waiting) {
        home_waiting = 0;
        uc_bt_auto_arm(&home_gate, now);
        home_connect_at = now;
        close_log();
        if (open_log()) {
            fprintf(stderr, "[bluetooth-auto] log source unavailable; manual start required\n");
            uc_bt_auto_cancel(&home_gate);
            return;
        }
        home_owns_log = 1;
        fprintf(stderr, "[bluetooth-auto] observing desktop for 120000ms; draining history\n");
    }
    if (now < home_gate.last || now >= home_gate.until) {
        fprintf(stderr, "[bluetooth-auto] observation expired; manual start required\n");
        uc_backend_auto_watch(0);
        return;
    }
    if (connection_pending) {
        struct sockaddr_in peer;
        socklen_t len = sizeof peer;
        if (getpeername(fd, (struct sockaddr *)&peer, &len)) {
            int error = 0;
            len = sizeof error;
            if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) && !error &&
                now - home_connect_at < 1000)
                return;
            if (raw_log_open()) {
                fprintf(stderr, "[bluetooth-auto] log connection failed; manual start required\n");
                uc_backend_auto_watch(0);
                return;
            }
        }
        connection_pending = 0;
    }
    int saved_phase = phase;
    int drained = read_log(0);
    phase = saved_phase; /* Observation failures do not corrupt output ownership. */
    if (fd < 0) {
        fprintf(stderr, "[bluetooth-auto] log stream lost; manual start required\n");
        uc_backend_auto_watch(0);
    } else if (drained && !home_gate.fresh) {
        used = 0;
        discard_line = 0;
        home_gate.fresh = 1;
        fprintf(stderr, "[bluetooth-auto] history drained; awaiting new desktop focus\n");
    }
}
int uc_backend_auto_due(int32_t target_user, int idle)
{
    if (auto_mode == UC_AUTO_RESUME) {
        /* The timer begins as soon as power is working; the final transport gate
         * independently enforces the same five-second power grace. */
        int eligible = idle && ready && power_status == 0 && !suspend_event;
        int due = uc_bt_auto_due(&home_gate, now_ms(), eligible);
        if (due || (!home_waiting && !home_gate.armed)) {
            uc_backend_auto_watch(UC_AUTO_OFF);
            if (due)
                fprintf(
                    stderr,
                    "[bluetooth-auto] resume delay complete; system-choice experimental start\n");
        }
        return due && uc_backend_transport_ready();
    }
    int32_t foreground = -1;
    int eligible = idle && home_owns_log && power_status == 0 && !suspend_event &&
                   sceUserServiceGetForegroundUser(&foreground) == 0 && user_live(foreground) &&
                   uc_backend_can_start(target_user);
    if (foreground != home_foreground) {
        home_gate.timing = 0;
        home_foreground = foreground;
    }
    int timing = home_gate.timing;
    int armed = home_gate.armed;
    int due = uc_bt_auto_due(&home_gate, now_ms(), eligible);
    if (!timing && home_gate.timing)
        fprintf(
            stderr,
            "[bluetooth-auto] desktop and users ready; waiting 5000ms foreground=%d target=%d\n",
            foreground, target_user);
    else if (timing && !home_gate.timing && !due && home_gate.armed)
        fprintf(stderr, "[bluetooth-auto] readiness lost; delay cancelled\n");
    if (due || (armed && !home_gate.armed)) {
        uc_backend_auto_watch(0);
        if (due)
            fprintf(stderr, "[bluetooth-auto] desktop delay complete; starting once\n");
    }
    return due;
}
static void clear_pending(void)
{
    close_log();
    phase = 0;
    owner = -1;
    device = 0;
    identified = 0;
    cancelled = 0;
    pending_slot = -1;
}
int uc_backend_create(int32_t user, uint64_t *handle)
{
    if (home_waiting || home_gate.armed || home_owns_log)
        uc_backend_auto_watch(0);
    if (!handle)
        return -1;
    *handle = 0;
    if (!ready || user < 0 || (user > 0 && !user_live(user)))
        return -1;
    if (power_status != 0) {
        backend_error(10, "create blocked: power state unknown or resting", power_status);
        return -1;
    }
    uint64_t current = now_ms();
    if (!working_since || current < working_since || current - working_since < 5000)
        return 2;
    for (unsigned i = 0; i < 4; i++)
        if (outputs[i].used && outputs[i].owner != user && (user == 0 || outputs[i].owner == 0))
            return -2;
    for (unsigned i = 0; i < 4; i++)
        if (outputs[i].used && outputs[i].owner == user) {
            if (outputs[i].closing)
                return -1;
            *handle = outputs[i].token;
            if (phase == 4 && pending_slot == (int)i)
                clear_pending(); /* Core has adopted this completed creation. */
            return 0;
        }
    if (phase && owner != user)
        return -2;
    if (phase == 5)
        return -1;
    if (!phase) {
        int slot = -1;
        for (unsigned i = 0; i < 4; i++)
            if (!outputs[i].used) {
                slot = (int)i;
                break;
            }
        if (slot < 0 || serial == UINT64_MAX)
            return -2;
        identified = 0;
        owner = user;
        cancelled = 0;
        matches = 0;
        device = 0;
        pending_slot = slot;
        started = now_ms();
        if (open_log()) {
            backend_error(5, "open_identification_log", errno);
            clear_pending();
            return -1;
        }
        phase = 1;
    }
    return 2;
}
static int neutral(uint64_t handle)
{
    int16_t axes[4] = {0};
    uint16_t triggers[2] = {0};
    return uc_backend_submit(handle, 0, axes, triggers);
}
int uc_backend_submit(uint64_t handle, uint32_t buttons, const int16_t axes[4],
                      const uint16_t triggers[2])
{
    owned_output *output = find_output(handle);
    if (!output || !axes || !triggers)
        return -1;
    int is_neutral = buttons == 0 && axes[0] == 0 && axes[1] == 0 && axes[2] == 0 && axes[3] == 0 &&
                     triggers[0] == 0 && triggers[1] == 0;
    if (!is_neutral &&
        (output->closing || power_status != 0 || (output->owner != 0 && !user_live(output->owner))))
        return -1;
    PadData d;
    memset(&d, 0, sizeof d);
    d.buttons = buttons;
    for (int i = 0; i < 4; i++)
        d.axes[i] = (uint8_t)((((int32_t)axes[i] + 32768) * 255 + 32767) / 65535);
    for (int i = 0; i < 2; i++)
        d.triggers[i] = (uint8_t)(((uint32_t)triggers[i] * 255 + 32767) / 65535);
    d.orientation[3] = 1;
    d.acceleration[1] = -1;
    d.connected = 1;
    d.count = 1;
    d.timestamp = now_ms() * 1000;
    int32_t rc = scePadVirtualDeviceInsertData((int32_t)output->device, &d);
    if (rc != 0)
        backend_error(6, "scePadVirtualDeviceInsertData", rc);
    else if (output->owner == 0 && !is_neutral) {
        int ps = (buttons & (UINT32_C(1) << 16)) != 0;
        if (!output->input_logged || (ps && !output->ps_logged))
            fprintf(stderr,
                    "[system-choice] device=%#llx input accepted buttons=%#x ps=%d; binding "
                    "unverified\n",
                    (unsigned long long)output->device, buttons, ps);
        output->input_logged = 1;
        output->ps_logged |= ps;
    }
    return rc == 0 ? 0 : -1;
}
int uc_backend_delete(uint64_t handle)
{
    if (handle) {
        owned_output *output = find_output(handle);
        if (!output)
            return -1;
        output->closing = 1;
        (void)neutral(handle);
        int32_t rc = scePadVirtualDeviceDeleteDevice((int32_t)output->device);
        if (rc != 0) {
            backend_error(7, "scePadVirtualDeviceDeleteDevice", rc);
            return -1;
        }
        int completed_pending = phase == 4 && pending_slot >= 0 && output == &outputs[pending_slot];
        memset(output, 0, sizeof(*output));
        if (completed_pending)
            clear_pending();
        return 0;
    }
    /* Zero never names a ready output. It only cancels the shared creation. */
    if (!phase)
        return 0;
    if (phase == 4) {
        if (pending_slot < 0 || pending_slot >= 4 || !outputs[pending_slot].used)
            return -1;
        return uc_backend_delete(outputs[pending_slot].token);
    }
    if (phase == 3) {
        cancelled = 1;
        return 2;
    }
    if (identified) {
        int32_t rc = scePadVirtualDeviceDeleteDevice((int32_t)device);
        if (rc != 0) {
            backend_error(7, "scePadVirtualDeviceDeleteDevice", rc);
            return -1;
        }
    }
    clear_pending();
    return 0;
}
int uc_backend_read_rumble(uint64_t handle, uint64_t now, uint8_t *strong, uint8_t *weak)
{
    if (strong)
        *strong = 0;
    if (weak)
        *weak = 0;
    owned_output *output = find_output(handle);
    if (!output || !strong || !weak)
        return -1;
    if (output->closing || (output->owner != 0 && !user_live(output->owner)) || power_status != 0)
        return 0;
    if (!remote_setting || output->feedback_failed)
        return 0;
    if (output->feedback_polled && now < output->feedback_at)
        return -1;
    if (output->feedback_polled && now - output->feedback_at < 16)
        return 0;
    output->feedback_polled = 1;
    output->feedback_at = now;
    /* Proven classic-motor layout from ps5_vpad.c c210fe6: keep the aligned
     * 256-byte remote-setting buffer, read motor bytes 3/4, ignore transient
     * update flags. No adaptive-trigger or haptic-audio ABI is inferred. */
    union {
        uint64_t align;
        uint8_t bytes[256];
    } setting = {0};
    int32_t rc = remote_setting((int32_t)output->device, setting.bytes);
    if (rc != 0) {
        output->feedback_failed = 1;
        backend_error(11, "scePadVirtualDeviceGetRemoteSetting", rc);
        return -1;
    }
    *strong = setting.bytes[3];
    *weak = setting.bytes[4];
    return 1;
}
static void observe_system_users(uint64_t now)
{
    int exists = 0;
    for (unsigned i = 0; i < 4; ++i)
        exists |= outputs[i].used && outputs[i].owner == 0;
    if (!exists) {
        system_users_valid = 0;
        system_users_at = 0;
        return;
    }
    if (system_users_at && now >= system_users_at && now - system_users_at < 1000)
        return;
    system_users_at = now;
    int32_t snapshot[5] = {-1, -1, -1, -1, -1};
    if (uc_backend_users(snapshot) || sceUserServiceGetForegroundUser(snapshot + 4))
        return;
    if (!system_users_valid || memcmp(snapshot, system_users_last, sizeof snapshot)) {
        memcpy(system_users_last, snapshot, sizeof snapshot);
        system_users_valid = 1;
        fprintf(
            stderr,
            "[system-choice] signed_in=%d,%d,%d,%d foreground=%d; not proof of device binding\n",
            snapshot[0], snapshot[1], snapshot[2], snapshot[3], snapshot[4]);
    }
}
void uc_backend_poll(void)
{
    uint64_t now = now_ms();
    if (last_poll && now - last_poll > 2000) {
        suspend_event = 1;
        working_since = 0;
    }
    int previous_power = power_status;
    last_poll = now;
    if (power_flag < 0)
        (void)sceKernelOpenEventFlag(&power_flag, "SceSystemStateMgrInfo");
    if (power_flag >= 0) {
        uint64_t bits = 0;
        if (sceKernelPollEventFlag(power_flag, UINT64_MAX, 2, &bits) == 0) {
            unsigned state = bits & 65535;
            power_status = state == 1000 ? 0 : ((state == 300 || state == 500) ? 1 : -1);
            if ((state == 300 || state == 500) && !rest_seen) {
                suspend_event = 1;
                rest_seen = 1;
            } else if (state == 1000)
                rest_seen = 0;
        } else
            power_status = -1;
    }
    if (power_status != previous_power)
        fprintf(stderr, "UniController: power %d -> %d; virtual output requires explicit enable\n",
                previous_power, power_status);
    if (power_status != 0)
        working_since = 0;
    else if (!working_since)
        working_since = now;
    observe_system_users(now);
    poll_home(now);
    if (!phase || phase == 4 || phase == 5)
        return;
    if (power_status != 0) {
        if (phase != 3)
            return;    /* Never initiate AddDevice during rest/unknown. */
        cancelled = 1; /* Identify only to remove our already pending device. */
    }
    if (now - started > 3500 && phase != 3) {
        phase = 5;
        close_log();
        return;
    }
    if (phase == 1) {
        if (connection_pending) {
            int err = 0;
            socklen_t len = sizeof err;
            struct sockaddr_in peer;
            socklen_t plen = sizeof peer;
            if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) {
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) {
                    if (raw_log_open()) {
                        phase = 5;
                        return;
                    }
                    phase = 2;
                } else
                    return;
            }
            connection_pending = 0;
        }
        phase = 2;
    }
    if (phase == 2 && read_log(0)) {
        used = 0;
        discard_line = 0;
        int32_t params[8];
        for (int i = 0; i < 8; i++)
            params[i] = (int32_t)0xdeadbeef;
        params[0] = sizeof params;
        params[1] = 1;
        int32_t add_result = scePadVirtualDeviceAddDevice(params, 3);
        if (add_result < 0) {
            backend_error(8, "scePadVirtualDeviceAddDevice", add_result);
            phase = 5;
            close_log();
            return;
        }
        phase = 3;
        started = now;
    }
    if (phase != 3)
        return;
    (void)read_log(1);
    if (phase != 3)
        return;
    now = now_ms(); /* read_log/candidate may advance the monotonic clock. */
    if (matches > 1 || (!matches && now - started >= 3000)) {
        fprintf(stderr, "UniController: identification refused, candidates=%d\n", matches);
        phase = 5;
        close_log();
        return;
    }
    if (matches == 1 && ambiguity_elapsed(now, matched)) {
        identified = 1;
        close_log();
        int32_t bind_result = 0;
        if (!cancelled && owner == 0 && power_status == 0) {
            fprintf(stderr,
                    "[system-choice] device=%#llx registered without application binding; press PS "
                    "to select user\n",
                    (unsigned long long)device);
        } else if (!cancelled && user_live(owner)) {
            bind_result = bind_user(device, owner);
            if (bind_result != 0)
                backend_error(9, "sceMbusBindDeviceWithUserId", bind_result);
        } else
            bind_result = -1;
        if (bind_result != 0) {
            if (scePadVirtualDeviceDeleteDevice((int32_t)device) == 0) {
                phase = cancelled ? 0 : 5;
                device = 0;
                identified = 0;
                owner = -1;
                pending_slot = -1;
            } else {
                backend_error(7, "scePadVirtualDeviceDeleteDevice", -1);
                phase = 5;
            }
            return;
        }
        if (pending_slot < 0 || pending_slot >= 4 || outputs[pending_slot].used ||
            serial == UINT64_MAX) {
            cancelled = 1;
            (void)uc_backend_delete(0);
            return;
        }
        owned_output *output = &outputs[pending_slot];
        memset(output, 0, sizeof(*output));
        output->used = 1;
        output->owner = owner;
        output->device = device;
        output->token = serial++;
        uint64_t handle = output->token;
        phase = 4; /* Still pending delivery until create returns the token. */
        /* The core repeats neutral before publishing its session. Keep this
         * owned handle discoverable if the first write fails, so that core
         * cleanup can retain/retry deletion rather than orphaning it. */
        (void)neutral(handle);
    }
}
int uc_backend_suspend(void)
{
    int event = suspend_event;
    suspend_event = 0;
    return event;
}
void uc_backend_shutdown(void)
{
    uc_backend_auto_watch(0);
    (void)uc_backend_delete(0);
    /* A pending AddDevice must finish identification before it can safely be
     * removed. This bounded shutdown grace never guesses an unrelated handle. */
    uint64_t deadline = now_ms() + 3500;
    while (phase == 3 && now_ms() < deadline) {
        uc_backend_poll();
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    (void)uc_backend_delete(0);
    if (phase == 5 && !identified)
        fprintf(stderr, "UniController: unidentified virtual device may require console restart\n");
    for (unsigned i = 0; i < 4; i++)
        if (outputs[i].used) {
            uint64_t handle = outputs[i].token;
            if (uc_backend_delete(handle) != 0)
                fprintf(stderr, "UniController: owned virtual output cleanup failed\n");
        }
    close_log();
    if (power_flag >= 0)
        sceKernelCloseEventFlag(power_flag);
    power_flag = -1;
    power_status = -1;
    if (raised && credentials(old_auth, old_caps))
        raised = 0;
    if (raised)
        fprintf(stderr, "UniController: original credential restoration failed\n");
    ready = 0;
}

int uc_backend_cancel_pending(void)
{
    return uc_backend_delete(0);
}

int uc_backend_power_state(void)
{
    return power_status;
}
