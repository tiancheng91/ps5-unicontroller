#include "device_options.h"
#define UC_JSON_TOKEN_MAX 1024
#include "../web/json.h"
#include "../logging.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

typedef struct {
    char key[UC_DEVICE_KEY_MAX];
    uc_device_options options;
} Entry;
static Entry entries[UC_DEVICE_OPTIONS_MAX];
static size_t count;
static char store[1024];
#define FILE_MAX 16384
static int valid_key(const char *key)
{
    if (!key || !*key || strlen(key) >= UC_DEVICE_KEY_MAX)
        return 0;
    for (const char *p = key; *p; ++p)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/._-", *p))
            return 0;
    return !strncmp(key, "usb:", 4) || !strncmp(key, "classic:", 8) || !strncmp(key, "ble:", 4);
}
static int string_key(Json *j, int n, char out[UC_DEVICE_KEY_MAX])
{
    if (n < 0 || j->t[n].kind != '"')
        return -1;
    int len = j->t[n].end - j->t[n].start - 2;
    if (len <= 0 || len >= UC_DEVICE_KEY_MAX)
        return -1;
    memcpy(out, j->s + j->t[n].start + 1, (size_t)len);
    out[len] = 0;
    return valid_key(out) ? 0 : -1;
}
int uc_device_options_load(const char *path)
{
    count = 0;
    store[0] = 0;
    if (!path || strlen(path) >= sizeof store)
        return -1;
    snprintf(store, sizeof store, "%s", path);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    char body[FILE_MAX + 1];
    size_t used = 0;
    while (used < sizeof body) {
        ssize_t n = read(fd, body + used, sizeof body - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (!n)
            break;
        used += (size_t)n;
    }
    close(fd);
    if (!used || used > FILE_MAX)
        return -1;
    body[used] = 0;
    Json j;
    const char *keys[] = {"schema_version", "devices"};
    int64_t version;
    if (jparse(&j, body, used) || jobject(&j, keys, 2) ||
        jint(&j, jget(&j, "schema_version"), 1, 1, &version))
        return -1;
    int array = jget(&j, "devices");
    if (array < 0 || j.t[array].kind != '[')
        return -1;
    Entry parsed[UC_DEVICE_OPTIONS_MAX];
    size_t total = 0;
    for (int n = array + 1; n < j.t[array].next; n = j.t[n].next) {
        if (total == UC_DEVICE_OPTIONS_MAX || j.t[n].kind != '{')
            return -1;
        /* Validate each object with the same strict duplicate/unknown-key rules. */
        Json item;
        const char *fields[] = {"key", "rumble", "swap_ab"};
        if (jparse(&item, body + j.t[n].start, (size_t)(j.t[n].end - j.t[n].start)) ||
            jobject(&item, fields, 3) || string_key(&item, jget(&item, "key"), parsed[total].key) ||
            jbool(&item, jget(&item, "rumble"), &parsed[total].options.rumble) ||
            jbool(&item, jget(&item, "swap_ab"), &parsed[total].options.swap_ab))
            return -1;
        for (size_t i = 0; i < total; ++i)
            if (!strcmp(parsed[i].key, parsed[total].key))
                return -1;
        ++total;
    }
    memcpy(entries, parsed, total * sizeof *entries);
    count = total;
    uc_log_info("[device-options] loaded entries=%zu", count);
    return 0;
}
uc_device_options uc_device_options_get(const char *key, int known)
{
    for (size_t i = 0; key && i < count; ++i)
        if (!strcmp(entries[i].key, key))
            return entries[i].options;
    return (uc_device_options){.rumble = known != 0, .swap_ab = 0};
}
int uc_device_options_save(const char *key, uc_device_options options)
{
    if (!store[0] || !valid_key(key) || (options.rumble != 0 && options.rumble != 1) ||
        (options.swap_ab != 0 && options.swap_ab != 1))
        return -1;
    size_t index = 0;
    while (index < count && strcmp(entries[index].key, key))
        ++index;
    if (index == UC_DEVICE_OPTIONS_MAX)
        return -1;
    char temporary[1100];
    snprintf(temporary, sizeof temporary, "%s.tmp", store);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    int failed = fprintf(file, "{\"schema_version\":1,\"devices\":[\n") < 0;
    size_t total = count + (index == count);
    for (size_t i = 0; i < total; ++i) {
        const char *name = i == index ? key : entries[i].key;
        uc_device_options value = i == index ? options : entries[i].options;
        if (fprintf(file, "%s  {\"key\":\"%s\",\"rumble\":%s,\"swap_ab\":%s}", i ? ",\n" : "", name,
                    value.rumble ? "true" : "false", value.swap_ab ? "true" : "false") < 0)
            failed = 1;
    }
    if (fprintf(file, "\n]}\n") < 0 || fflush(file) || fsync(fd))
        failed = 1;
    if (fclose(file))
        failed = 1;
    if (failed || rename(temporary, store)) {
        unlink(temporary);
        return -1;
    }
    snprintf(entries[index].key, sizeof entries[index].key, "%s", key);
    entries[index].options = options;
    count = total;
    uc_log_info("[device-options] saved key=%s rumble=%d swap_ab=%d", key, options.rumble,
                options.swap_ab);
    return 0;
}
uint32_t uc_device_options_buttons(uint32_t buttons, int swap)
{
    if (!swap)
        return buttons;
    return (buttons & ~UINT32_C(0xF000)) | ((buttons & (UINT32_C(1) << 14)) >> 1) |
           ((buttons & (UINT32_C(1) << 13)) << 1) | ((buttons & (UINT32_C(1) << 15)) >> 3) |
           ((buttons & (UINT32_C(1) << 12)) << 3);
}
