#include "profiles.h"
#include "nintendo.h"
#include "../config/overrides.h"
#include "crc32.h"
#include "util.h"

#include <string.h>

/* Pinned, unmodified SDL/Valve tables. No SDL library/runtime dependency. */
#include "../../../vendor/sdl/controller_type.h"
#include "../../../vendor/sdl/controller_list.h"

/* Trigger travel past which L2/R2 also count as pressed buttons. */
#define TRIGGER_PRESS 0x20

/* ---- shared helpers ------------------------------------------------------ */

/* A hat switch: 0 north, clockwise to 7 north-west; anything else is centred. */
static uint32_t hat_buttons(unsigned hat)
{
    static const uint32_t dirs[8] = {
        PAD_UP,   PAD_UP | PAD_RIGHT,  PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
        PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT,  PAD_UP | PAD_LEFT,
    };
    return hat < 8 ? dirs[hat] : 0;
}

static uint32_t trigger_buttons(uint8_t l2, uint8_t r2)
{
    return (l2 > TRIGGER_PRESS ? PAD_L2 : 0) | (r2 > TRIGGER_PRESS ? PAD_R2 : 0);
}

/* The three button bytes DualShock 4 and DualSense share:
 *   b0: hat (low nibble), square, cross, circle, triangle
 *   b1: L1, R1, L2, R2, share/create, options, L3, R3
 *   b2: PS, touchpad click, (DualSense) mute */
static uint32_t sony_buttons(const uint8_t *b)
{
    static const uint32_t b0[4] = {PAD_SQUARE, PAD_CROSS, PAD_CIRCLE, PAD_TRIANGLE};
    static const uint32_t b1[8] = {PAD_L1,     PAD_R1,      PAD_L2, PAD_R2,
                                   PAD_CREATE, PAD_OPTIONS, PAD_L3, PAD_R3};
    uint32_t out = hat_buttons(b[0] & 0x0F);
    int i;

    for (i = 0; i < 4; i++)
        if (b[0] & (0x10 << i))
            out |= b0[i];
    for (i = 0; i < 8; i++)
        if (b[1] & (1 << i))
            out |= b1[i];
    if (b[2] & 0x01)
        out |= PAD_PS;
    if (b[2] & 0x02)
        out |= PAD_TOUCHPAD;
    return out;
}

/* A touch point: contact byte (bit 7 set = no finger), then 12-bit x and y
 * packed in three bytes. */
static void sony_touch(const uint8_t *p, pad_state *st, int i)
{
    st->touch[i].down = !(p[0] & 0x80);
    st->touch[i].x = (uint16_t)(p[1] | (p[2] & 0x0F) << 8);
    st->touch[i].y = (uint16_t)(p[2] >> 4 | p[3] << 4);
}

static void read_motion(const uint8_t *gyro, const uint8_t *accel, pad_state *st)
{
    int i;

    for (i = 0; i < 3; i++) {
        st->gyro[i] = les16(gyro + 2 * i);
        st->accel[i] = les16(accel + 2 * i);
    }
    st->has_motion = 1;
}

static void sony_basic(const uint8_t *sticks, const uint8_t *buttons, uint8_t l2, uint8_t r2,
                       pad_state *st)
{
    st->lx = sticks[0];
    st->ly = sticks[1];
    st->rx = sticks[2];
    st->ry = sticks[3];
    st->l2 = l2;
    st->r2 = r2;
    st->buttons = sony_buttons(buttons) | trigger_buttons(l2, r2);
}

/* ---- DualShock 4 ----------------------------------------------------------
 *
 * Over Bluetooth it starts with the short report 0x01 and moves to the full
 * report 0x11 once it receives an output report 0x11.
 *
 *   0x01: [1..4] sticks  [5..7] buttons  [8] L2  [9] R2
 *   0x11: [1..2] flags   [3..6] sticks   [7..9] buttons  [10] L2  [11] R2
 *         [15..20] gyro  [21..26] accel  [32] battery (low nibble level,
 *         bit 4 cable)   [35] touch report count, [36] first touch report:
 *         timestamp, then two 4-byte points
 */

static int ds4_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    (void)ctx;
    if (r[0] == 0x01 && n >= 10) {
        sony_basic(r + 1, r + 5, r[8], r[9], st);
        return PARSE_OK;
    }
    if (r[0] != 0x11 || n < 36)
        return PARSE_NONE;

    sony_basic(r + 3, r + 7, r[10], r[11], st);
    read_motion(r + 15, r + 21, st);

    {
        unsigned level = r[32] & 0x0F, cable = (r[32] >> 4) & 1;
        unsigned full = cable ? 11 : 8; /* cable reports up to 11 */
        if (level > full)
            level = full;
        st->battery_pct = (uint8_t)(level * 100 / full);
        st->charging = (uint8_t)(cable && level < full);
        st->has_battery = 1;
    }
    if (n >= 45 && r[35] > 0) {
        sony_touch(r + 37, st, 0);
        sony_touch(r + 41, st, 1);
        st->has_touch = 1;
    }
    return PARSE_FULL;
}

/* Output report 0x11: 73 bytes after the id, then the CRC.
 *   [1] 0xC0 | report interval  [2] 0x20  [3] 0xF3 (rumble, light bar)
 *   [4] 0x04  [6] weak motor  [7] strong motor  [8..10] light bar RGB */
static int ds4_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    const int len = 1 + 73 + 4;

    (void)ctx;
    if (max < len)
        return 0;
    memset(b, 0, (size_t)len);
    b[0] = 0x11;
    b[1] = 0xC0 | 0x08; /* 0x08: 125 Hz input reports */
    b[2] = 0x20;
    b[3] = 0xF3;
    b[4] = 0x04;
    b[6] = o->weak;
    b[7] = o->strong;
    b[8] = o->r;
    b[9] = o->g;
    b[10] = o->b;
    crc32_seal_output(b, len);
    return len;
}

/* Waking a Sony pad up is just sending it an output report. */
static int ds4_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    return step == 0 ? ds4_output(ctx, o, b, max) : 0;
}

/* ---- DualSense --------------------------------------------------------------
 *
 * Same idea: short report 0x01 until an output report 0x31 arrives.
 *
 *   0x01: [1..4] sticks  [5..8] buttons  [9] L2  [10] R2
 *   0x31: [2..5] sticks  [6] L2  [7] R2  [9..12] buttons
 *         [17..22] gyro  [23..28] accel  [34..41] two touch points
 *         [54] battery: low nibble level 0..10, high nibble 1 charging,
 *         2 full
 */

static int ds5_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    (void)ctx;
    if (r[0] == 0x01 && n >= 11) {
        sony_basic(r + 1, r + 5, r[9], r[10], st);
        return PARSE_OK;
    }
    if (r[0] != 0x31 || n < 55)
        return PARSE_NONE;

    sony_basic(r + 2, r + 9, r[6], r[7], st);
    read_motion(r + 17, r + 23, st);
    sony_touch(r + 34, st, 0);
    sony_touch(r + 38, st, 1);
    st->has_touch = 1;

    {
        unsigned level = r[54] & 0x0F, status = r[54] >> 4;
        unsigned pct = level * 10 + 5;
        st->battery_pct = (uint8_t)(pct > 100 ? 100 : pct);
        st->charging = (uint8_t)(status == 1);
        st->has_battery = 1;
    }
    return PARSE_FULL;
}

/* Output report 0x31: 73 bytes after the id, then the CRC.
 *   [1] 0x02 (tag)  [2] valid flags 0: rumble  [3] valid flags 1: lights
 *   [4] weak motor  [5] strong motor  [40] valid flags 2
 *   [43] light bar setup  [44] LED brightness  [45] player LEDs
 *   [46..48] light bar RGB */
static int ds5_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    static const uint8_t player_leds[5] = {0x00, 0x04, 0x0A, 0x15, 0x1B};
    const int len = 1 + 73 + 4;

    (void)ctx;
    if (max < len)
        return 0;
    memset(b, 0, (size_t)len);
    b[0] = 0x31;
    b[1] = 0x02;
    b[2] = 0x03;
    b[3] = 0x54;
    b[4] = o->weak;
    b[5] = o->strong;
    b[40] = 0x03;
    b[43] = 0x02;
    b[44] = 0x02;
    b[45] = 0x20 | player_leds[o->player < 5 ? o->player : 4]; /* 0x20: no fade-in */
    b[46] = o->r;
    b[47] = o->g;
    b[48] = o->b;
    crc32_seal_output(b, len);
    return len;
}

static int ds5_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    return step == 0 ? ds5_output(ctx, o, b, max) : 0;
}

/* ---- Xbox One / Series ------------------------------------------------------
 *
 * The same reports over Bluetooth Classic (older firmware) and Bluetooth LE
 * (HID over GATT, current firmware), as documented publicly:
 *
 *   0x01: [1..8] sticks, 16-bit each  [9..12] triggers, 10-bit each
 *         [13] hat, 1 north .. 8 north-west, 0 centred
 *         16 bytes (first Xbox One S firmware):
 *           [14] A B X Y LB RB View Menu  [15] L3 R3
 *         17 bytes and more:
 *           [14] A B - X Y - LB RB  [15] - - View Menu Xbox L3 R3
 *           [16] bit 0: Share on Series controllers, View on older ones
 *   0x02: [1] bit 0 Xbox button (first firmware)
 *   0x04: [1] battery: bits 0-1 level, bits 2-3 power mode, bit 4 charging
 *
 * View is the "select" of Xbox games: it becomes the touchpad click, which
 * plays that part in PlayStation games. Share becomes Create.
 */

typedef struct {
    int has_share;
} xbox_ctx;

static void xbox_setup(void *ctx, uint16_t vid, uint16_t pid)
{
    xbox_ctx *c = ctx;

    c->has_share = vid == 0x045E && (pid == 0x0B12 || pid == 0x0B13);
}

static int xbox_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    const xbox_ctx *c = ctx;
    uint32_t b = 0;
    unsigned hat;

    if (r[0] == 0x02 && n >= 2) {
        st->buttons = (st->buttons & ~(uint32_t)PAD_PS) | (r[1] & 1 ? PAD_PS : 0);
        return PARSE_OK;
    }
    if (r[0] == 0x04 && n >= 2) {
        st->battery_pct = (uint8_t)((r[1] & 3) * 100 / 3);
        st->charging = (uint8_t)((r[1] >> 4) & 1);
        st->has_battery = 1;
        return PARSE_OK;
    }
    if (r[0] != 0x01 || n < 16)
        return PARSE_NONE;

    st->lx = (uint8_t)(le16(r + 1) >> 8);
    st->ly = (uint8_t)(le16(r + 3) >> 8);
    st->rx = (uint8_t)(le16(r + 5) >> 8);
    st->ry = (uint8_t)(le16(r + 7) >> 8);
    st->l2 = (uint8_t)((le16(r + 9) & 0x3FF) >> 2);
    st->r2 = (uint8_t)((le16(r + 11) & 0x3FF) >> 2);

    hat = r[13];
    b |= hat >= 1 && hat <= 8 ? hat_buttons(hat - 1) : 0;

    if (n > 16) {
        if (r[14] & 0x01)
            b |= PAD_CROSS;
        if (r[14] & 0x02)
            b |= PAD_CIRCLE;
        if (r[14] & 0x08)
            b |= PAD_SQUARE;
        if (r[14] & 0x10)
            b |= PAD_TRIANGLE;
        if (r[14] & 0x40)
            b |= PAD_L1;
        if (r[14] & 0x80)
            b |= PAD_R1;
        if (r[15] & 0x04)
            b |= PAD_TOUCHPAD; /* View */
        if (r[15] & 0x08)
            b |= PAD_OPTIONS; /* Menu */
        if (r[15] & 0x10)
            b |= PAD_PS; /* Xbox */
        if (r[15] & 0x20)
            b |= PAD_L3;
        if (r[15] & 0x40)
            b |= PAD_R3;
        if (r[16] & 0x01)
            b |= c && c->has_share ? PAD_CREATE : PAD_TOUCHPAD;
    } else {
        if (r[14] & 0x01)
            b |= PAD_CROSS;
        if (r[14] & 0x02)
            b |= PAD_CIRCLE;
        if (r[14] & 0x04)
            b |= PAD_SQUARE;
        if (r[14] & 0x08)
            b |= PAD_TRIANGLE;
        if (r[14] & 0x10)
            b |= PAD_L1;
        if (r[14] & 0x20)
            b |= PAD_R1;
        if (r[14] & 0x40)
            b |= PAD_TOUCHPAD; /* View */
        if (r[14] & 0x80)
            b |= PAD_OPTIONS; /* Menu */
        if (r[15] & 0x01)
            b |= PAD_L3;
        if (r[15] & 0x02)
            b |= PAD_R3;
        b |= st->buttons & PAD_PS; /* comes in report 0x02 */
    }
    st->buttons = b | trigger_buttons(st->l2, st->r2);
    return PARSE_OK;
}

/* Output report 0x03, as sent over Bluetooth Classic and LE (the same
 * bytes as SDL's HIDAPI Xbox One driver and xpadneo): all four motors
 * enabled, trigger motors, main motors (strong = left/low, weak =
 * right/high, 0..100), on for 2.55 s, no delay, repeated 235 times: in
 * effect until the next report. Over LE it is written to the output Report
 * characteristic whose Report Reference says id 3, type output. A test
 * pulse runs pulse_10ms and is not repeated, so it stops by itself even
 * if the report that ends it is lost. */
static int xbox_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    (void)ctx;
    if (max < 9)
        return 0;
    b[0] = 0x03;
    b[1] = 0x0F;
    b[2] = 0;
    b[3] = 0;
    b[4] = (uint8_t)(o->strong * 100 / 255);
    b[5] = (uint8_t)(o->weak * 100 / 255);
    b[6] = o->pulse_10ms ? o->pulse_10ms : 0xFF;
    b[7] = 0x00;
    b[8] = o->pulse_10ms ? 0x00 : 0xEB;
    return 9;
}

/* Local exceptions precede SDL family hints; capabilities stay transport-specific. */
#include "controller_registry.inc"

static const ControllerDescription_t *sdl_device(uint16_t vid, uint16_t pid)
{
    unsigned id = ((unsigned)vid << 16) | pid;
    for (size_t i = 0; i < sizeof arrControllers / sizeof arrControllers[0]; i++)
        if (arrControllers[i].m_unDeviceID == id)
            return &arrControllers[i];
    return NULL;
}
static EControllerType sdl_type(uint16_t vid, uint16_t pid)
{
    const ControllerDescription_t *device = sdl_device(vid, pid);
    return device ? device->m_eControllerType : k_eControllerType_UnknownNonSteamController;
}

const pad_profile *profile_find(uint16_t vid, uint16_t pid)
{
    size_t p;
    int i;

    for (p = 0; p + 1 < sizeof k_no_rumble / sizeof k_no_rumble[0]; p++)
        if (k_no_rumble[p].vid == vid && k_no_rumble[p].pid == pid)
            return &k_no_rumble[p].profile;

    for (p = 0; p < sizeof k_profiles / sizeof k_profiles[0]; p++)
        for (i = 0; i < k_profiles[p].nids; i++)
            if (k_profiles[p].ids[i][0] == vid && k_profiles[p].ids[i][1] == pid)
                return &k_profiles[p];
    return NULL;
}

const char *profile_model_name(uint16_t vid, uint16_t pid)
{
    size_t i;

    for (i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid && k_models[i].name)
            return k_models[i].name;
    const ControllerDescription_t *device = sdl_device(vid, pid);
    return device ? device->m_pszName : NULL;
}

const char *pad_display_name(uint16_t vid, uint16_t pid, const char *profile)
{
    const char *m = profile_model_name(vid, pid);

    if (m)
        return m;
    if (profile && *profile && strcmp(profile, "Generic HID") != 0)
        return profile;
    return "Controller";
}

int profile_class_is_gamepad(uint32_t cod)
{
    unsigned major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x0F;

    return major == 5 && (minor == 1 || minor == 2);
}

profile_family profile_classify(uint16_t vid, uint16_t pid)
{
    const pad_profile *profile = profile_find(vid, pid);
    if (!profile)
        return PROFILE_GENERIC_HID;
    if (profile->parse == switch_parse || profile->parse == joycon_left_parse ||
        profile->parse == joycon_right_parse)
        return PROFILE_SWITCH;
    if (profile->parse == xbox_parse)
        return PROFILE_XBOX;
    return PROFILE_KNOWN_OTHER;
}

int profile_usb_switch_pro(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid)
            return k_models[i].usb_switch_pro;
    return 0;
}

int profile_usb_input_watchdog(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid)
            return k_models[i].usb_input_watchdog;
    return 0;
}
const char *profile_hid_mapping(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid)
            return k_models[i].map;
    return NULL;
}

const pad_profile *profile_builtin_named(const char *key)
{
    if (!key)
        return NULL;
    for (size_t i = 0; i < sizeof k_profiles / sizeof k_profiles[0]; i++)
        if (strcmp(key, k_profile_keys[i]) == 0)
            return &k_profiles[i];
    return NULL;
}
const pad_profile *profile_find_for_transport(pad_transport transport, uint16_t vid, uint16_t pid)
{
    const void *selected = NULL;
    if ((transport == PAD_TRANSPORT_CLASSIC || transport == PAD_TRANSPORT_BLE) &&
        uc_controller_override_profile(transport, vid, pid, &selected))
        return selected;
    return profile_find(vid, pid);
}
/* SDL classifies a family, not a universal wire protocol. Use this mapping
 * only on Classic HID links. In particular Xbox 360/XInput USB and Xbox One
 * USB GIP cannot select the Xbox Bluetooth parser merely from their ID. */
static const pad_profile *sdl_classic_candidate(uint16_t vid, uint16_t pid, int *probe)
{
    switch (sdl_type(vid, pid)) {
    case k_eControllerType_SwitchProController:
    /* SDL's input-only distinction applies to USB. Its Switch HIDAPI driver
     * uses the full Pro wireless protocol for third-party Bluetooth devices.
     * Still require device-info and full-input validation on this Classic path.
     * https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c */
    case k_eControllerType_SwitchInputOnlyController:
        if (probe)
            *probe = 1;
        return profile_builtin_named("switch");
    case k_eControllerType_SwitchJoyConLeft:
        return profile_builtin_named("joycon-left");
    case k_eControllerType_SwitchJoyConRight:
        return profile_builtin_named("joycon-right");
    case k_eControllerType_XBoxOneController:
    case k_eControllerType_XBoxEliteController:
        return profile_builtin_named("xbox");
    case k_eControllerType_PS4Controller:
        return profile_builtin_named("ds4");
    case k_eControllerType_PS5Controller:
    case k_eControllerType_PS5EdgeController:
        return profile_builtin_named("ds5");
    default:
        return NULL;
    }
}
int profile_classic_unsupported(uint16_t vid, uint16_t pid)
{
    if (profile_find(vid, pid) || profile_hid_mapping(vid, pid))
        return 0; /* Local, maintained protocol/layout exceptions win. */
    if (sdl_classic_candidate(vid, pid, NULL))
        return 0;
    switch (sdl_type(vid, pid)) {
    case k_eControllerType_Unknown:
    case k_eControllerType_UnknownNonSteamController:
    case k_eControllerType_AndroidController:
    case k_eControllerType_AppleController:
        return 0; /* Standard HID still needs a usable descriptor. */
    default:
        return 1; /* Known proprietary or USB-only family without a Classic codec. */
    }
}

/* The host enforces authentication and native-device exclusion before selection.
 * A single most-specific matching rule wins (fields are not merged across rules).
 * SDL-style builtin order: zero-ID exact names, placeholder IDs, then the
 * known VID/PID table. Classification selects a candidate, not verified input.
 * Verified-bond fallback and the generic HID descriptor path live in host.c. */
const pad_profile *profile_classic_candidate(uint16_t vid, uint16_t pid, const uint8_t address[6],
                                             const char *name, int *probe)
{
    if (probe)
        *probe = 0;
    if (profile_system_bluetooth(vid, pid))
        return NULL;
    const uc_controller_override *rule =
        uc_controller_override_identity(PAD_TRANSPORT_CLASSIC, vid, pid, address, name);
    if (rule && rule->has_profile) {
        const pad_profile *p =
            !strcmp(rule->profile, "generic-hid") ? NULL : profile_builtin_named(rule->profile);
        if (probe && p == profile_builtin_named("switch"))
            *probe = 1;
        return p;
    }
    if (rule && rule->has_mapping)
        return NULL; /* Validated raw mappings retain the generic decoder. */
    return profile_classic_default(vid, pid, name, probe);
}

/* Also used by configuration validation, without consulting active overrides. */
const pad_profile *profile_classic_default(uint16_t vid, uint16_t pid, const char *name, int *probe)
{
    if (probe)
        *probe = 0;
    /* SDL_GetGamepadTypeFromVIDPID handles zero IDs before its VID/PID table.
     * These exact names are shared by licensed/compatible models; none proves
     * a vendor or protocol. Require the bounded Nintendo probe for every match.
     * This rule applies only to Classic, never grants BLE access or USB support.
     * https://github.com/libsdl-org/SDL/blob/main/src/joystick/SDL_joystick.c */
    if (!vid && !pid) {
        static const char *const switch_names[] = {
            "Lic Pro Controller",
            "Nintendo Wireless Gamepad",
            "Wireless Gamepad",
        };
        for (size_t i = 0; name && i < sizeof switch_names / sizeof switch_names[0]; i++) {
            if (!strcmp(name, switch_names[i])) {
                if (probe)
                    *probe = 1;
                return profile_builtin_named("switch");
            }
        }
        return NULL;
    }
    /* SDL treats this placeholder identity as standard, not an Xbox/Switch ID. */
    if (vid == 1 && pid == 1)
        return NULL;
    const pad_profile *local = profile_find(vid, pid);
    if (local || profile_hid_mapping(vid, pid))
        return local; /* A maintained generic HID layout wins over SDL inference. */
    return sdl_classic_candidate(vid, pid, probe);
}
profile_family profile_classify_for_transport(pad_transport transport, uint16_t vid, uint16_t pid)
{
    const pad_profile *p = profile_find_for_transport(transport, vid, pid);
    if (!p)
        return PROFILE_GENERIC_HID;
    if (p->parse == switch_parse || p->parse == joycon_left_parse || p->parse == joycon_right_parse)
        return PROFILE_SWITCH;
    if (p->parse == xbox_parse)
        return PROFILE_XBOX;
    return PROFILE_KNOWN_OTHER;
}
const char *pad_display_name_for_transport(pad_transport transport, uint16_t vid, uint16_t pid,
                                           const char *profile)
{
    const uc_controller_override *r = uc_controller_override_find(transport, vid, pid);
    return r && r->has_name ? r->name : pad_display_name(vid, pid, profile);
}

int profile_device_known(pad_transport transport, uint16_t vid, uint16_t pid)
{
    /* A display label from SDL is not evidence of supported protocol/output. */
    for (size_t i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid && k_models[i].name)
            return 1;
    return profile_find_for_transport(transport, vid, pid) != NULL;
}

int profile_system_bluetooth(uint16_t vid, uint16_t pid)
{
    return vid == 0x054c && (pid == 0x0ce6 || pid == 0x0df2);
}

int profile_ble_allowed(uint16_t vid, uint16_t pid)
{
    if (profile_system_bluetooth(vid, pid))
        return 0;
    /* Use the vendored SDL classification instead of a separate Nintendo PID
     * denylist. Unlisted devices (e.g. 057e:2073 in this snapshot) fall through
     * to the normal BLE allowlist/override policy; an absent SDL entry neither
     * grants access nor proves compatibility. */
    EControllerType type = sdl_type(vid, pid);
    if (type == k_eControllerType_SwitchProController ||
        type == k_eControllerType_SwitchInputOnlyController ||
        type == k_eControllerType_SwitchJoyConLeft || type == k_eControllerType_SwitchJoyConRight ||
        type == k_eControllerType_SwitchJoyConPair ||
        type == k_eControllerType_Switch2ProController ||
        type == k_eControllerType_Switch2InputOnlyController)
        return 0;
    if (profile_classify(vid, pid) == PROFILE_SWITCH ||
        profile_classify_for_transport(PAD_TRANSPORT_BLE, vid, pid) == PROFILE_SWITCH)
        return 0;
    const uc_controller_override *rule = uc_controller_override_find(PAD_TRANSPORT_BLE, vid, pid);
    if (rule && rule->has_ble)
        return rule->ble;
    for (size_t i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid)
            return k_models[i].ble;
    return 0;
}
