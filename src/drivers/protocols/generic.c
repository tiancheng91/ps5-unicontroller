/* Generic Bluetooth HID input decoded from the device report descriptor.
 * The descriptor locates axes, hats and buttons in each report. Default
 * button ordering is a compatibility heuristic, not a HID or DirectInput
 * standard. Device-specific layouts come from the controller registry.
 */
#include "profiles.h"
#include "../config/overrides.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

enum {
    AX_X,
    AX_Y,
    AX_Z,
    AX_RX,
    AX_RY,
    AX_RZ,
    AX_SLIDER,
    AX_DIAL,
    AX_BRAKE,
    AX_ACCEL,
    AX_HAT,
    AX_N,
    AX_NONE = 0xFF
};

enum { ROLE_LX, ROLE_LY, ROLE_RX, ROLE_RY, ROLE_L2, ROLE_R2, ROLE_N };

#define MAX_BUTTONS 32
#define MAX_REPORT_BITS (512 * 8) /* bits in one input report */

typedef struct {
    uint8_t present, rid;
    uint16_t off, size;
    int32_t min, max;
} field;

typedef struct {
    int has_ids;
    field axis[AX_N];
    field button[MAX_BUTTONS]; /* usage n at index n - 1 */
    field home, back;          /* consumer page: AC Home, AC Back */
    int nbuttons;

    uint8_t role[ROLE_N]; /* AX_* per role */
    uint32_t button_map[MAX_BUTTONS];
} generic_ctx;

_Static_assert(sizeof(generic_ctx) <= PROFILE_CTX_MAX, "generic context too large");

/* ---- the report descriptor ---------------------------------------------- */

typedef struct {
    uint16_t page;
    int32_t lmin, lmax;
    uint32_t size, count;
    uint8_t rid;
} hid_globals;

static int32_t item_value(const uint8_t *p, int size, int is_signed)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    if (is_signed && size && size < 4 && (v & (1u << (8 * size - 1))))
        v |= ~0u << (8 * size);
    return (int32_t)v;
}

static void put_field(generic_ctx *g, uint32_t usage, uint16_t page, const hid_globals *gl,
                      uint16_t off)
{
    field f;
    uint16_t pg = usage >> 16 ? (uint16_t)(usage >> 16) : page;
    uint16_t u = (uint16_t)usage;
    field *slot = NULL;

    f.present = 1;
    f.rid = gl->rid;
    f.off = off;
    f.size = (uint16_t)gl->size;
    f.min = gl->lmin;
    f.max = gl->lmax;

    if (pg == 0x01 && u >= 0x30 && u <= 0x37)
        slot = &g->axis[AX_X + (u - 0x30)];
    else if (pg == 0x01 && u == 0x39)
        slot = &g->axis[AX_HAT];
    else if (pg == 0x02 && u == 0xC5)
        slot = &g->axis[AX_BRAKE];
    else if (pg == 0x02 && u == 0xC4)
        slot = &g->axis[AX_ACCEL];
    else if (pg == 0x09 && u >= 1 && u <= MAX_BUTTONS) {
        slot = &g->button[u - 1];
        if (u > g->nbuttons)
            g->nbuttons = u;
    } else if (pg == 0x0C && u == 0x223)
        slot = &g->home;
    else if (pg == 0x0C && u == 0x224)
        slot = &g->back;

    if (slot && !slot->present)
        *slot = f; /* the first one found wins */
}

/* Walks the descriptor's items, placing every input field it recognises.
 * Returns 0 if the descriptor is malformed. */
static int parse_descriptor(generic_ctx *g, const uint8_t *d, int n)
{
    hid_globals gl, stack[4];
    int depth = 0, i = 0;
    uint32_t usages[64], umin = 0, umax = 0;
    int nusages = 0, has_range = 0;
    uint16_t offset[256];

    memset(&gl, 0, sizeof gl);
    memset(offset, 0, sizeof offset);

    while (i < n) {
        uint8_t p = d[i];
        int size, type, tag;
        const uint8_t *v;
        int32_t val;

        if (p == 0xFE) { /* long item */
            if (i + 2 >= n)
                return 0;
            i += 3 + d[i + 1];
            continue;
        }
        size = (p & 3) == 3 ? 4 : (p & 3);
        type = (p >> 2) & 3;
        tag = p >> 4;
        if (i + 1 + size > n)
            return 0;
        v = d + i + 1;
        val = item_value(v, size, type == 1 && (tag == 1 || tag == 3));
        i += 1 + size;

        if (type == 1) { /* global */
            switch (tag) {
            case 0:
                gl.page = (uint16_t)val;
                break;
            case 1:
                gl.lmin = val;
                break;
            case 2:
                /* A positive maximum is often written in fewer bytes than
                 * its sign bit would need: read it unsigned then. */
                gl.lmax = gl.lmin >= 0 ? (int32_t)item_value(v, size, 0) : val;
                break;
            case 7:
                gl.size = (uint32_t)val;
                break;
            case 8:
                gl.rid = (uint8_t)val;
                g->has_ids = 1;
                break;
            case 9:
                gl.count = (uint32_t)val;
                break;
            case 10:
                if (depth < 4)
                    stack[depth++] = gl;
                break;
            case 11:
                if (depth > 0)
                    gl = stack[--depth];
                break;
            default:
                break;
            }
        } else if (type == 2) { /* local */
            uint32_t u = size == 4 ? (uint32_t)val : (uint32_t)val & 0xFFFF;
            if (tag == 0 && nusages < 64)
                usages[nusages++] = u;
            else if (tag == 1) {
                umin = u;
                has_range = 1;
            } else if (tag == 2) {
                umax = u;
                has_range = 1;
            }
        } else if (type == 0) { /* main */
            if (tag == 8) {     /* input */
                uint32_t k, bits;
                uint16_t *off = &offset[gl.rid];
                /* A report is at most a few hundred bytes: anything larger
                 * is a malformed descriptor, refused before it costs time. */
                if (gl.size == 0 || gl.size > 32 || gl.count > MAX_REPORT_BITS)
                    return 0;
                bits = gl.count * gl.size;
                if (*off + bits > MAX_REPORT_BITS)
                    return 0;
                if (!(val & 1)) { /* data, not constant padding */
                    for (k = 0; k < gl.count; k++) {
                        uint32_t u;
                        if (nusages)
                            u = usages[k < (uint32_t)nusages ? k : (uint32_t)nusages - 1];
                        else if (has_range)
                            u = umin + k > umax ? umax : umin + k;
                        else
                            break;
                        put_field(g, u, gl.page, &gl, (uint16_t)(*off + k * gl.size));
                    }
                }
                *off = (uint16_t)(*off + bits);
            } else if (tag == 9 || tag == 11) { /* output, feature: other reports */
            }
            nusages = 0;
            has_range = 0;
            umin = umax = 0;
        }
    }
    return 1;
}

/* ---- the mapping --------------------------------------------------------- */

static const struct {
    const char *name;
    uint32_t bit;
} k_buttons[] = {
    {"cross", PAD_CROSS},
    {"circle", PAD_CIRCLE},
    {"square", PAD_SQUARE},
    {"triangle", PAD_TRIANGLE},
    {"l1", PAD_L1},
    {"r1", PAD_R1},
    {"l2", PAD_L2},
    {"r2", PAD_R2},
    {"l3", PAD_L3},
    {"r3", PAD_R3},
    {"create", PAD_CREATE},
    {"share", PAD_CREATE},
    {"options", PAD_OPTIONS},
    {"ps", PAD_PS},
    {"touchpad", PAD_TOUCHPAD},
    {"up", PAD_UP},
    {"down", PAD_DOWN},
    {"left", PAD_LEFT},
    {"right", PAD_RIGHT},
    {"none", 0},
};

static const char *const k_axes[AX_N] = {
    "x", "y", "z", "rx", "ry", "rz", "slider", "dial", "brake", "accel", "hat",
};

static const char *const k_roles[ROLE_N] = {"lx", "ly", "rx", "ry", "l2", "r2"};

static void default_mapping(generic_ctx *g)
{
    static const uint32_t android[15] = {
        PAD_CROSS, PAD_CIRCLE, 0,          PAD_SQUARE,  PAD_TRIANGLE, 0,      PAD_L1, PAD_R1,
        PAD_L2,    PAD_R2,     PAD_CREATE, PAD_OPTIONS, PAD_PS,       PAD_L3, PAD_R3,
    };
    static const uint32_t dinput[14] = {
        PAD_SQUARE, PAD_CROSS,  PAD_CIRCLE,  PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2,
        PAD_R2,     PAD_CREATE, PAD_OPTIONS, PAD_L3,       PAD_R3, PAD_PS, PAD_TOUCHPAD,
    };
    const field *a = g->axis;
    int i;

    memset(g->button_map, 0, sizeof g->button_map);
    if (g->nbuttons >= 15)
        for (i = 0; i < 15; i++)
            g->button_map[i] = android[i];
    else
        for (i = 0; i < 14; i++)
            g->button_map[i] = dinput[i];

    memset(g->role, AX_NONE, sizeof g->role);
    if (a[AX_X].present)
        g->role[ROLE_LX] = AX_X;
    if (a[AX_Y].present)
        g->role[ROLE_LY] = AX_Y;

    if (a[AX_Z].present && a[AX_RZ].present) {
        g->role[ROLE_RX] = AX_Z;
        g->role[ROLE_RY] = AX_RZ;
        if (a[AX_RX].present && a[AX_RY].present && !a[AX_BRAKE].present) {
            g->role[ROLE_L2] = AX_RX;
            g->role[ROLE_R2] = AX_RY;
        }
    } else if (a[AX_RX].present && a[AX_RY].present) {
        g->role[ROLE_RX] = AX_RX;
        g->role[ROLE_RY] = AX_RY;
        if (a[AX_Z].present && !a[AX_BRAKE].present)
            g->role[ROLE_L2] = AX_Z;
        if (a[AX_RZ].present && !a[AX_ACCEL].present)
            g->role[ROLE_R2] = AX_RZ;
    }
    if (a[AX_BRAKE].present)
        g->role[ROLE_L2] = AX_BRAKE;
    if (a[AX_ACCEL].present)
        g->role[ROLE_R2] = AX_ACCEL;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* Applies one validated registry binding. Returns 0 for an unknown binding. */
static int map_line(generic_ctx *g, char *line)
{
    char *eq, *key, *val;
    unsigned n;
    size_t i;

    line = trim(line);
    if (!*line || *line == '#')
        return 1;
    eq = strchr(line, '=');
    if (!eq)
        return 0;
    *eq = '\0';
    key = trim(line);
    val = trim(eq + 1);

    if (sscanf(key, "button%u", &n) == 1 && n >= 1 && n <= MAX_BUTTONS) {
        for (i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
            if (strcmp(val, k_buttons[i].name) == 0) {
                g->button_map[n - 1] = k_buttons[i].bit;
                return 1;
            }
        return 0;
    }
    for (i = 0; i < ROLE_N; i++) {
        size_t a;
        if (strcmp(key, k_roles[i]) != 0)
            continue;
        if (strcmp(val, "none") == 0) {
            g->role[i] = AX_NONE;
            return 1;
        }
        for (a = 0; a < AX_N; a++)
            if (strcmp(val, k_axes[a]) == 0) {
                g->role[i] = (uint8_t)a;
                return 1;
            }
        return 0;
    }
    return 0;
}

/* Default descriptor mapping, then registry layout, then optional local override. */
static void apply_mapping(generic_ctx *g, const char *mapping)
{
    if (!mapping)
        return;
    char buf[1024], *line, *next;
    snprintf(buf, sizeof buf, "%s", mapping);
    for (line = buf; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        (void)map_line(g, line);
    }
}

int generic_setup_for_transport(void *ctx, const uint8_t *desc, int len, uint16_t vid, uint16_t pid,
                                pad_transport transport)
{
    generic_ctx *g = ctx;

    memset(g, 0, sizeof *g);
    if (!parse_descriptor(g, desc, len)) {
        log_line("generic %04x:%04x: malformed HID descriptor", vid, pid);
        return 0;
    }
    if (!g->axis[AX_X].present && !g->axis[AX_HAT].present && g->nbuttons < 4) {
        log_line("generic %04x:%04x: no gamepad controls in the descriptor", vid, pid);
        return 0;
    }
    default_mapping(g);
    apply_mapping(g, profile_hid_mapping(vid, pid));
    const uc_controller_override *override = uc_controller_override_find(transport, vid, pid);
    if (override && override->has_mapping)
        apply_mapping(g, override->mapping);
    log_line("generic %04x:%04x: %d buttons, axes%s%s%s%s%s%s%s%s, hat %s", vid, pid, g->nbuttons,
             g->axis[AX_X].present ? " x" : "", g->axis[AX_Y].present ? " y" : "",
             g->axis[AX_Z].present ? " z" : "", g->axis[AX_RX].present ? " rx" : "",
             g->axis[AX_RY].present ? " ry" : "", g->axis[AX_RZ].present ? " rz" : "",
             g->axis[AX_BRAKE].present ? " brake" : "", g->axis[AX_ACCEL].present ? " accel" : "",
             g->axis[AX_HAT].present ? "yes" : "no");
    return 1;
}

/* ---- reading reports ------------------------------------------------------- */

static int32_t get_bits(const uint8_t *d, int len, const field *f)
{
    uint32_t v = 0;
    int i;

    if ((f->off + f->size + 7) / 8 > len || f->size == 0 || f->size > 32)
        return f->min;
    for (i = 0; i < f->size; i++) {
        int bit = f->off + i;
        if (d[bit >> 3] & (1 << (bit & 7)))
            v |= 1u << i;
    }
    if (f->min < 0 && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size; /* sign-extend */
    return (int32_t)v;
}

static uint8_t to_byte(const field *f, int32_t v)
{
    int64_t span = (int64_t)f->max - f->min;
    int64_t s;

    if (span <= 0)
        return 128;
    s = ((int64_t)v - f->min) * 255 / span;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

/* Reads one report. A pad may spread its controls over several reports
 * (the home button often has its own), so only the controls this report
 * carries are updated. */
static int generic_parse(void *ctx, const uint8_t *rep, int len, pad_state *st)
{
    static const uint32_t dirs[8] = {
        PAD_UP,   PAD_UP | PAD_RIGHT,  PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
        PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT,  PAD_UP | PAD_LEFT,
    };
    generic_ctx *g = ctx;
    const uint8_t *d = rep;
    uint8_t rid = 0;
    uint8_t *out[ROLE_N] = {&st->lx, &st->ly, &st->rx, &st->ry, &st->l2, &st->r2};
    uint32_t b = 0, mask = 0;
    int i, used = 0, trig[2] = {0, 0};

    if (g->has_ids) {
        if (len < 2)
            return PARSE_NONE;
        rid = rep[0];
        d = rep + 1;
        len--;
    }

    for (i = 0; i < ROLE_N; i++) {
        const field *f;
        uint8_t v;
        if (g->role[i] == AX_NONE)
            continue;
        f = &g->axis[g->role[i]];
        if (!f->present || f->rid != rid)
            continue;
        v = to_byte(f, get_bits(d, len, f));
        *out[i] = v;
        if (i == ROLE_L2)
            trig[0] = 1;
        if (i == ROLE_R2)
            trig[1] = 1;
        used = 1;
    }
    for (i = 0; i < g->nbuttons; i++) {
        const field *f = &g->button[i];
        if (!f->present || f->rid != rid)
            continue;
        mask |= g->button_map[i];
        if (get_bits(d, len, f))
            b |= g->button_map[i];
        used = 1;
    }
    if (g->axis[AX_HAT].present && g->axis[AX_HAT].rid == rid) {
        const field *f = &g->axis[AX_HAT];
        int32_t h = get_bits(d, len, f) - f->min;
        mask |= PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
        if (h >= 0 && h < 8)
            b |= dirs[h];
        used = 1;
    }
    if (g->home.present && g->home.rid == rid) {
        mask |= PAD_PS;
        if (get_bits(d, len, &g->home))
            b |= PAD_PS;
        used = 1;
    }
    if (g->back.present && g->back.rid == rid) {
        mask |= PAD_CREATE;
        if (get_bits(d, len, &g->back))
            b |= PAD_CREATE;
        used = 1;
    }
    if (!used)
        return PARSE_NONE;

    /* Analog triggers also press L2/R2; digital-only ones fill the axis. */
    if (trig[0]) {
        mask |= PAD_L2;
        if (st->l2 > 0x20)
            b |= PAD_L2;
    }
    if (trig[1]) {
        mask |= PAD_R2;
        if (st->r2 > 0x20)
            b |= PAD_R2;
    }
    st->buttons = (st->buttons & ~mask) | b;
    if (g->role[ROLE_L2] == AX_NONE && (mask & PAD_L2))
        st->l2 = b & PAD_L2 ? 255 : 0;
    if (g->role[ROLE_R2] == AX_NONE && (mask & PAD_R2))
        st->r2 = b & PAD_R2 ? 255 : 0;
    return PARSE_OK;
}

const pad_profile generic_profile = {
    "Generic HID", NULL, 0, generic_parse, NULL, NULL, 0, 0, NULL,
};

int generic_setup(void *ctx, const uint8_t *desc, int len, uint16_t vid, uint16_t pid)
{
    return generic_setup_for_transport(ctx, desc, len, vid, pid, PAD_TRANSPORT_UNSPECIFIED);
}
