/* Nintendo Switch 1 protocol shared by Classic and the existing USB parser.
 * USB-specific handshakes and Switch 2 remain in usb/protocol.c.
 * Protocol reference: Linux hid-nintendo.c, joycon_read_info,
 * joycon_read_stick_calibration and joycon_parse_report.
 * https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c
 */
#include "nintendo.h"
#include "../common/util.h"
#include <string.h>

static uint32_t hat_buttons(unsigned hat)
{
    static const uint32_t dirs[8] = {
        PAD_UP,   PAD_UP | PAD_RIGHT,  PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
        PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT,  PAD_UP | PAD_LEFT,
    };
    return hat < 8 ? dirs[hat] : 0;
}

/* ---- Nintendo Switch Pro Controller and compatibles -------------------------
 *
 * Starts in a simple HID mode (report 0x3F) and moves to the standard full
 * report 0x30 when asked with subcommands in output report 0x01:
 *
 *   0x01: [1] packet counter (low nibble)  [2..9] rumble data
 *         [10] subcommand id  [11..] subcommand arguments
 *
 *   0x30: [1] timer  [2] battery (high nibble) and connection
 *         [3] right buttons: Y X B A SR SL R ZR
 *         [4] shared: - + RS LS Home Capture
 *         [5] left buttons: down up right left SR SL L ZL
 *         [6..8] left stick, [9..11] right stick: two 12-bit values each
 *         [13..48] three IMU samples: accel xyz, gyro xyz, 16-bit each
 *
 *   0x3F: [1] B A Y X L R ZL ZR  [2] - + LS RS Home Capture  [3] hat
 *         [4..11] sticks, 16-bit each
 *
 * Buttons are placed by position, as on the PlayStation: B (bottom) is
 * cross, A (right) circle, Y (left) square, X (top) triangle.
 */

typedef struct {
    uint8_t counter, controller_type, info_valid, hori_xy, calibration_pending;
    uint16_t calibration[4][3]; /* center, negative range, positive range */
} switch_ctx;
_Static_assert(sizeof(switch_ctx) <= PROFILE_CTX_MAX, "Nintendo context exceeds profile storage");

void switch_setup(void *ctx, uint16_t vid, uint16_t pid)
{
    if (!ctx)
        return;
    switch_ctx *c = ctx;
    memset(c, 0, sizeof *c);
    c->controller_type = NINTENDO_TYPE_PRO;
    if (vid == 0x057e) {
        switch (pid) {
        case 0x2006:
            c->controller_type = NINTENDO_TYPE_JOYCON_LEFT;
            break;
        case 0x2007:
            c->controller_type = NINTENDO_TYPE_JOYCON_RIGHT;
            break;
        case 0x2017:
            c->controller_type = NINTENDO_TYPE_SNES;
            break;
        case 0x2019:
            c->controller_type = NINTENDO_TYPE_N64;
            break;
        case 0x201a:
        case 0x201e:
            c->controller_type = NINTENDO_TYPE_GENESIS;
            break;
        default:
            break;
        }
    } else if ((vid == 0x0f0d && pid == 0x00f6) || (vid == 0x0e6f && pid == 0x018c)) {
        c->controller_type = NINTENDO_TYPE_LICENSED_PRO;
        c->hori_xy = vid == 0x0f0d;
    }
}
static unsigned controller_type(const switch_ctx *c)
{
    return c && c->controller_type ? c->controller_type : NINTENDO_TYPE_PRO;
}
static int has_left_stick(unsigned type)
{
    return type == 1 || type == 3 || type == 6 || type == 12;
}
static int has_right_stick(unsigned type)
{
    return type == 2 || type == 3 || type == 6;
}
static unsigned packed(const uint8_t *p, unsigned y)
{
    return y ? (p[1] >> 4) | (unsigned)p[2] << 4 : p[0] | (unsigned)(p[1] & 15) << 8;
}
/* Read-only factory calibration, with different left/right field order as in
 * Linux joycon_read_stick_calibration. Licensed controllers keep defaults since
 * their flash layout may differ. A malformed reply never changes calibration. */
static void calibration_reply(switch_ctx *c, const uint8_t *r, int n)
{
    if (!c || !c->info_valid || controller_type(c) == 6 || n < 29 || n > 64 || r[13] != 0x90 ||
        r[14] != 0x10 || r[19] != 9)
        return;
    uint32_t address =
        (uint32_t)r[15] | (uint32_t)r[16] << 8 | (uint32_t)r[17] << 16 | (uint32_t)r[18] << 24;
    unsigned side;
    if (address == 0x603d)
        side = 0;
    else if (address == 0x6046)
        side = 1;
    else
        return;
    if (!(c->calibration_pending & (1u << side)))
        return;
    uint16_t cal[2][3];
    for (unsigned axis = 0; axis < 2; axis++) {
        unsigned center = packed(r + 20 + (side ? 0 : 3), axis);
        unsigned negative = packed(r + 20 + (side ? 3 : 6), axis);
        unsigned positive = packed(r + 20 + (side ? 6 : 0), axis);
        if (!center || center >= 4095 || !negative || !positive || negative > center ||
            positive > 4095 - center)
            return;
        cal[axis][0] = (uint16_t)center;
        cal[axis][1] = (uint16_t)negative;
        cal[axis][2] = (uint16_t)positive;
    }
    memcpy(c->calibration + side * 2, cal, sizeof cal);
    c->calibration_pending &= (uint8_t) ~(1u << side);
}
static int device_info(switch_ctx *c, const uint8_t *r, int n)
{
    if (!c || n < 26 || n > 64 || r[0] != 0x21 || r[13] != 0x82 || r[14] != 2)
        return 0;
    switch (r[17]) {
    case 1:
    case 2:
    case 3:
    case 6:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
        break;
    default:
        return 0;
    }
    if (c->controller_type != r[17]) {
        memset(c->calibration, 0, sizeof c->calibration);
        c->calibration_pending = 0;
    }
    c->controller_type = r[17];
    c->info_valid = 1;
    return 1;
}

/* Protocol amplitude levels (milli-units), from dekuNukem's rumble data:
 * github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md
 * Also used by Linux hid-nintendo. Fixed 160/320 Hz avoids libm and
 * keeps the encoded amplitude at or below the normal 1.003 maximum. */
static void switch_motor(uint8_t strength, uint8_t b[4])
{
    static const uint16_t amplitudes[] = {
        0,   8,   12,  14,  17,  20,  24,  28,  33,  40,  47,  56,  67,  80,  95,  112,  117,
        123, 128, 134, 140, 146, 152, 159, 166, 173, 181, 189, 198, 206, 215, 225, 230,  235,
        240, 245, 251, 256, 262, 268, 273, 279, 286, 292, 298, 305, 311, 318, 325, 332,  340,
        347, 355, 362, 370, 378, 387, 395, 404, 413, 422, 431, 440, 450, 460, 470, 480,  491,
        501, 512, 524, 535, 547, 559, 571, 584, 596, 609, 623, 636, 650, 665, 679, 694,  709,
        725, 741, 757, 773, 790, 808, 825, 843, 862, 881, 900, 920, 940, 960, 981, 1003,
    };
    unsigned amplitude = strength * 1000u / 255u;
    unsigned i = 0;

    while (i + 1 < sizeof amplitudes / sizeof amplitudes[0] && amplitudes[i] < amplitude)
        i++;
    b[0] = 0x00; /* 320 Hz high band */
    b[1] = (uint8_t)(0x01 + 2 * i);
    b[2] = (uint8_t)(0x40 + ((i & 1) << 7)); /* 160 Hz low band */
    b[3] = (uint8_t)(0x40 + (i >> 1));
}

void switch_encode_rumble(uint8_t *b, uint8_t counter, uint8_t strong, uint8_t weak)
{
    b[0] = 0x10;
    b[1] = counter & 0x0F;
    switch_motor(strong, b + 2); /* left actuator */
    switch_motor(weak, b + 6);   /* right actuator */
}

int switch_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    switch_ctx *c = ctx;

    unsigned type = controller_type(c);
    if (!c || !o || !b || max < 10 || type == 9 || type == 10 || type == 11 || type == 13)
        return 0;
    switch_encode_rumble(b, c->counter++, o->strong, o->weak);
    return 10;
}

/* Stick travel from centre the raw values usually reach; factory
 * calibration (SPI flash) is not read yet. */
#define SWITCH_STICK_RANGE 1600

static uint8_t switch_axis(const uint16_t *cal, int v, int invert)
{
    int center = cal && cal[0] && cal[1] && cal[2] ? cal[0] : 2048;
    int range = cal && cal[0] && cal[1] && cal[2] ? cal[v < center ? 1 : 2] : SWITCH_STICK_RANGE;
    int s = (v - center) * 127 / range;

    if (invert)
        s = -s;
    s += 128;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

static void switch_stick(const switch_ctx *c, unsigned side, const uint8_t *p, uint8_t *x,
                         uint8_t *y)
{
    *x = switch_axis(c ? c->calibration[side * 2] : NULL, (int)packed(p, 0), 0);
    *y = switch_axis(c ? c->calibration[side * 2 + 1] : NULL, (int)packed(p, 1), 1);
}

int switch_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    uint32_t b = 0;

    switch_ctx *c = ctx;
    if (!r || !st || n < 1)
        return PARSE_NONE;
    if (r[0] == 0x21) {
        if (n >= 15) {
            (void)device_info(c, r, n);
            calibration_reply(c, r, n);
        }
        return PARSE_NONE; /* Metadata must never submit an input frame. */
    }
    if (r[0] == 0x3F && n >= 12) {
        if (controller_type(c) >= NINTENDO_TYPE_NES_LEFT)
            return PARSE_NONE;
        pad_state_reset(st);
        if (r[1] & 0x01)
            b |= PAD_CROSS; /* B */
        if (r[1] & 0x02)
            b |= PAD_CIRCLE; /* A */
        if (r[1] & 0x04)
            b |= PAD_SQUARE; /* Y */
        if (r[1] & 0x08)
            b |= PAD_TRIANGLE; /* X */
        if (r[1] & 0x10)
            b |= PAD_L1;
        if (r[1] & 0x20)
            b |= PAD_R1;
        if (r[1] & 0x40)
            b |= PAD_L2;
        if (r[1] & 0x80)
            b |= PAD_R2;
        if (r[2] & 0x01)
            b |= PAD_CREATE;
        if (r[2] & 0x02)
            b |= PAD_OPTIONS;
        if (r[2] & 0x04)
            b |= PAD_L3;
        if (r[2] & 0x08)
            b |= PAD_R3;
        if (r[2] & 0x10)
            b |= PAD_PS;
        if (r[2] & 0x20)
            b |= PAD_TOUCHPAD; /* Capture */
        b |= hat_buttons(r[3]);
        st->lx = (uint8_t)(le16(r + 4) >> 8);
        st->ly = (uint8_t)(le16(r + 6) >> 8);
        st->rx = (uint8_t)(le16(r + 8) >> 8);
        st->ry = (uint8_t)(le16(r + 10) >> 8);
        st->l2 = b & PAD_L2 ? 255 : 0;
        st->r2 = b & PAD_R2 ? 255 : 0;
        st->buttons = b;
        return PARSE_OK;
    }
    if (r[0] != 0x30 || n < 49 || n > 64)
        return PARSE_NONE;
    pad_state_reset(st);

    if (r[3] & 0x01)
        b |= PAD_SQUARE; /* Y */
    if (r[3] & 0x02)
        b |= PAD_TRIANGLE; /* X */
    if (r[3] & 0x04)
        b |= PAD_CROSS; /* B */
    if (r[3] & 0x08)
        b |= PAD_CIRCLE; /* A */
    if (r[3] & 0x40)
        b |= PAD_R1;
    if (r[3] & 0x80)
        b |= PAD_R2;
    if (r[4] & 0x01)
        b |= PAD_CREATE; /* - */
    if (r[4] & 0x02)
        b |= PAD_OPTIONS; /* + */
    if (r[4] & 0x04)
        b |= PAD_R3;
    if (r[4] & 0x08)
        b |= PAD_L3;
    if (r[4] & 0x10)
        b |= PAD_PS; /* Home */
    if (r[4] & 0x20)
        b |= PAD_TOUCHPAD; /* Capture */
    if (r[5] & 0x01)
        b |= PAD_DOWN;
    if (r[5] & 0x02)
        b |= PAD_UP;
    if (r[5] & 0x04)
        b |= PAD_RIGHT;
    if (r[5] & 0x08)
        b |= PAD_LEFT;
    if (r[5] & 0x40)
        b |= PAD_L1;
    if (r[5] & 0x80)
        b |= PAD_L2;
    switch_stick(c, 0, r + 6, &st->lx, &st->ly);
    switch_stick(c, 1, r + 9, &st->rx, &st->ry);
    unsigned type = controller_type(c);
    if (!has_left_stick(type))
        st->lx = st->ly = 128;
    if (!has_right_stick(type))
        st->rx = st->ry = 128;
    if (c && c->hori_xy) {
        uint32_t square = b & PAD_SQUARE, triangle = b & PAD_TRIANGLE;
        b &= ~(PAD_SQUARE | PAD_TRIANGLE);
        if (square)
            b |= PAD_TRIANGLE;
        if (triangle)
            b |= PAD_SQUARE;
    }
    /* Linux's physical button bits; targets are PS equivalents. N64 C buttons
     * use a digital right stick because PS has no four extra C button codes. */
    if (type == 9 || type == 10) {
        b &= PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1 | PAD_CREATE | PAD_OPTIONS;
        if (r[3] & 8)
            b |= PAD_CROSS;
        if (r[3] & 4)
            b |= PAD_CIRCLE;
    } else if (type == 13) {
        b &= PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_OPTIONS | PAD_PS | PAD_TOUCHPAD;
        if (r[3] & 8)
            b |= PAD_SQUARE; /* A */
        if (r[3] & 4)
            b |= PAD_CROSS; /* B */
        if (r[3] & 0x40)
            b |= PAD_CIRCLE; /* C */
        if (r[3] & 2)
            b |= PAD_L1; /* X, six-button variant */
        if (r[3] & 1)
            b |= PAD_TRIANGLE; /* Y */
        if (r[5] & 0x40)
            b |= PAD_R1; /* Z */
        if (r[3] & 0x80)
            b |= PAD_CREATE; /* Mode */
    } else if (type == 12) {
        b &= PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1 | PAD_L2 | PAD_OPTIONS |
             PAD_PS | PAD_TOUCHPAD;
        if (r[3] & 8)
            b |= PAD_CROSS;
        if (r[3] & 4)
            b |= PAD_CIRCLE;
        if (r[4] & 8)
            b |= PAD_R2; /* physical ZR uses L-stick-click bit */
        int horizontal = !!(r[4] & 1) - !!(r[3] & 2);
        int vertical = !!(r[3] & 0x80) - !!(r[3] & 1);
        st->rx = horizontal < 0 ? 0 : horizontal > 0 ? 255 : 128;
        st->ry = vertical < 0 ? 0 : vertical > 0 ? 255 : 128;
    }
    st->l2 = b & PAD_L2 ? 255 : 0; /* ZL/ZR are digital */
    st->r2 = b & PAD_R2 ? 255 : 0;
    st->buttons = b;

    {
        unsigned level = r[2] >> 5; /* 0 empty .. 4 full */
        st->battery_pct = level <= 4 ? (uint8_t)(level * 25) : 0;
        st->charging = (uint8_t)((r[2] >> 4) & 1);
        st->has_battery = level <= 4;
    }
    st->has_motion = 0;
    if (n >= 25 && (type == 1 || type == 2 || type == 3 || type == 6)) {
        /* First IMU sample: accel then gyro, 16-bit each. */
        int i;
        for (i = 0; i < 3; i++) {
            st->accel[i] = les16(r + 13 + 2 * i);
            st->gyro[i] = les16(r + 19 + 2 * i);
        }
        st->has_motion = 1;
    }
    return PARSE_FULL;
}

/* An output report 0x01 carrying one subcommand, rumble neutral. */
static int switch_subcmd(switch_ctx *c, uint8_t id, const uint8_t *arg, int alen, uint8_t *b,
                         int max)
{
    static const uint8_t neutral[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

    if (max < 11 + alen)
        return 0;
    b[0] = 0x01;
    b[1] = c->counter++ & 0x0F;
    memcpy(b + 2, neutral, 8);
    b[10] = id;
    memcpy(b + 11, arg, (size_t)alen);
    return 11 + alen;
}

/* Device info layout and licensed type follow Linux joycon_read_info and
 * JOYCON_CTLR_TYPE_LIC_PRO in hid-nintendo.c (see the source URL below).
 * This is a read-only protocol check, not Bluetooth pairing/authentication. */
int profile_switch_info_request(void *ctx, uint8_t *out, int capacity)
{
    if (!ctx || !out)
        return 0;
    static const uint8_t empty[1] = {0};
    return switch_subcmd(ctx, 2, empty, 0, out, capacity);
}
int profile_switch_info_reply(void *ctx, const uint8_t *r, int n, uint8_t *type)
{
    if (!ctx || !r || n < 15 || r[0] != 0x21 || r[14] != 2)
        return 0;
    if (!(r[13] & 0x80))
        return -1;
    if (n < 26 || n > 64)
        return 0;
    if (r[13] != 0x82 || (r[17] != 3 && r[17] != 6))
        return -1;
    (void)device_info(ctx, r, n);
    if (type)
        *type = r[17];
    return 1;
}
/* Optional factory reads follow the stable input setup. Missing metadata or
 * calibration falls back to defaults; no SPI write/reset/pairing commands. */
int switch_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    if (!ctx || !o || !b || step < 0)
        return 0;
    switch_ctx *c = ctx;
    unsigned type = controller_type(c);
    int index = 0;
    static const uint8_t full_mode[1] = {0x30}, on[1] = {1};
    if (step == index++)
        return profile_switch_info_request(ctx, b, max);
    if (step == index++)
        return switch_subcmd(c, 3, full_mode, 1, b, max);
    if (type == 1 || type == 2 || type == 3 || type == 6) {
        if (step == index++)
            return switch_subcmd(c, 0x40, on, 1, b, max);
    }
    if (has_left_stick(type) || has_right_stick(type)) {
        if (step == index++)
            return switch_subcmd(c, 0x48, on, 1, b, max);
    }
    if (step == index++) {
        unsigned player = o->player >= 1 && o->player <= 4 ? o->player : 1;
        uint8_t leds = (uint8_t)(1u << (player - 1));
        return switch_subcmd(c, 0x30, &leds, 1, b, max);
    }
    if (c->info_valid && type != NINTENDO_TYPE_LICENSED_PRO) {
        for (unsigned side = 0; side < 2; side++) {
            if (!(side ? has_right_stick(type) : has_left_stick(type)))
                continue;
            if (step == index++) {
                uint32_t address = side ? 0x6046 : 0x603d;
                uint8_t args[5] = {(uint8_t)address, (uint8_t)(address >> 8), 0, 0, 9};
                int length = switch_subcmd(c, 0x10, args, sizeof args, b, max);
                if (length)
                    c->calibration_pending |= (uint8_t)(1u << side);
                return length;
            }
        }
    }
    return 0;
}

/* Standalone halves retain their physical orientation; no automatic combining.
 * A missing stick must remain centred even when its wire bytes are all zero. */
int joycon_left_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    int result = switch_parse(ctx, r, n, st);
    if (result != PARSE_NONE)
        st->rx = st->ry = 128;
    return result;
}
int joycon_right_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    int result = switch_parse(ctx, r, n, st);
    if (result != PARSE_NONE)
        st->lx = st->ly = 128;
    return result;
}
