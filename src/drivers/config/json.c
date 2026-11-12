#define UC_JSON_TOKEN_MAX 4096
#include "overrides.h"
#include "../../web/json.h"
#include <stdio.h>

static int text(Json *j, int n, char *out, size_t cap)
{
    if (n < 0 || j->t[n].kind != '"')
        return -1;
    int len = j->t[n].end - j->t[n].start - 2;
    if (len < 1 || (size_t)len >= cap)
        return -1;
    const char *s = j->s + j->t[n].start + 1;
    for (int i = 0; i < len; i++)
        if (s[i] < 32 || s[i] > 126 || s[i] == '\\')
            return -1;
    memcpy(out, s, (size_t)len);
    out[len] = 0;
    return 0;
}
static int object(Json *j, int n, const char *const *keys, int count)
{
    if (n < 0 || j->t[n].kind != '{')
        return -1;
    uint64_t seen = 0;
    for (int k = n + 1; k < j->t[n].next; k = j->t[k + 1].next) {
        int i;
        for (i = 0; i < count; i++)
            if (jeq(j, k, keys[i]))
                break;
        if (i == count || (seen & (UINT64_C(1) << i)))
            return -1;
        seen |= UINT64_C(1) << i;
    }
    return 0;
}
static int get(Json *j, int n, const char *key)
{
    if (n < 0)
        return -1;
    for (int k = n + 1; k < j->t[n].next; k = j->t[k + 1].next)
        if (jeq(j, k, key))
            return k + 1;
    return -1;
}
static int hexid(Json *j, int n, uint16_t *out)
{
    char s[5];
    if (text(j, n, s, sizeof s) || strlen(s) != 4)
        return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        const char *p = strchr("0123456789abcdef", (s[i] >= 'A' && s[i] <= 'F') ? s[i] + 32 : s[i]);
        if (!p)
            return -1;
        v = v * 16 + (unsigned)(p - "0123456789abcdef");
    }
    *out = (uint16_t)v;
    return 0;
}
static int binding(const char *key, const char *value)
{
    const char *roles[] = {"lx", "ly", "rx", "ry", "l2", "r2"};
    const char *axes[] = {"x",      "y",    "z",     "rx",    "ry",          "rz",
                          "slider", "dial", "brake", "accel", "accelerator", "none"};
    const char *buttons[] = {"cross",    "circle", "square", "triangle", "l1",    "r1",      "l2",
                             "r2",       "l3",     "r3",     "create",   "share", "options", "ps",
                             "touchpad", "up",     "down",   "left",     "right", "none"};
    int button = 0;
    unsigned b = 0;
    char tail;
    if (sscanf(key, "button%u%c", &b, &tail) == 1 && b >= 1 && b <= 32) {
        char normalized[16];
        snprintf(normalized, sizeof normalized, "button%u", b);
        button = !strcmp(key, normalized);
    }
    int role = 0;
    for (size_t i = 0; i < 6; i++)
        role |= !strcmp(key, roles[i]);
    if (!button && !role)
        return 0;
    const char *const *values = button ? buttons : axes;
    size_t count = button ? 20 : 12;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(value, values[i]))
            return 1;
    return 0;
}
int uc_controller_json_parse(const unsigned char *data, size_t size, uc_controller_overrides *out,
                             char *error, size_t capacity)
{
    static const char *const rootkeys[] = {"schema_version", "devices"},
                             *const entrykeys[] = {"match", "name", "profile", "mapping", "ble"},
                             *const matchkeys[] = {"transport", "vid", "pid"};
    (void)jobject;
    (void)jget;
    memset(out, 0, sizeof *out);
    Json *j = calloc(1, sizeof *j);
    int ok = 0;
    if (!j || !data || !size || size > UC_OVERRIDE_FILE_MAX)
        goto done;
    if (jparse(j, (const char *)data, size) || object(j, 0, rootkeys, 2))
        goto done;
    int64_t version;
    if (jint(j, get(j, 0, "schema_version"), 1, 1, &version))
        goto done;
    int list = get(j, 0, "devices");
    if (list < 0 || j->t[list].kind != '[')
        goto done;
    for (int n = list + 1; n < j->t[list].next; n = j->t[n].next) {
        if (out->count == UC_OVERRIDE_MAX || object(j, n, entrykeys, 5))
            goto done;
        uc_controller_override *e = &out->devices[out->count];
        int m = get(j, n, "match");
        if (object(j, m, matchkeys, 3) || hexid(j, get(j, m, "vid"), &e->vid) ||
            hexid(j, get(j, m, "pid"), &e->pid))
            goto done;
        char transport[16];
        if (text(j, get(j, m, "transport"), transport, sizeof transport))
            goto done;
        e->transport = !strcmp(transport, "classic") ? PAD_TRANSPORT_CLASSIC
                       : !strcmp(transport, "ble")   ? PAD_TRANSPORT_BLE
                       : !strcmp(transport, "usb")   ? PAD_TRANSPORT_USB
                                                     : 0;
        if (!e->transport)
            goto done;
        int t = get(j, n, "ble");
        if (t >= 0) {
            if (e->transport != PAD_TRANSPORT_BLE || jbool(j, t, &e->ble))
                goto done;
            e->has_ble = 1;
        }
        t = get(j, n, "name");
        if (t >= 0) {
            if (text(j, t, e->name, sizeof e->name))
                goto done;
            e->has_name = 1;
        }
        t = get(j, n, "profile");
        if (t >= 0) {
            if (text(j, t, e->profile, sizeof e->profile))
                goto done;
            e->has_profile = 1;
        }
        t = get(j, n, "mapping");
        if (t >= 0) {
            if (j->t[t].kind != '{' || j->t[t].next == t + 1)
                goto done;
            size_t used = 0;
            for (int k = t + 1; k < j->t[t].next; k = j->t[k + 1].next) {
                char key[16], value[16];
                if (text(j, k, key, sizeof key) || text(j, k + 1, value, sizeof value) ||
                    !binding(key, value))
                    goto done;
                for (int q = t + 1; q < k; q = j->t[q + 1].next)
                    if (jeq(j, q, key))
                        goto done;
                int len =
                    snprintf(e->mapping + used, sizeof e->mapping - used, "%s=%s\n", key, value);
                if (len < 0 || (size_t)len >= sizeof e->mapping - used)
                    goto done;
                used += (size_t)len;
            }
            e->has_mapping = 1;
        }
        if (!e->has_name && !e->has_profile && !e->has_mapping && !e->has_ble)
            goto done;
        for (size_t i = 0; i < out->count; i++)
            if (out->devices[i].transport == e->transport && out->devices[i].vid == e->vid &&
                out->devices[i].pid == e->pid)
                goto done;
        out->count++;
    }
    ok = 1;
done:
    free(j);
    if (!ok) {
        memset(out, 0, sizeof *out);
        if (error && capacity)
            snprintf(error, capacity, "invalid controller JSON schema or limits");
        return -1;
    }
    if (error && capacity)
        *error = 0;
    return 0;
}
