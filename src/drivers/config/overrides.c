#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "overrides.h"
#include "../protocols/profiles.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Immutable during active inputs; each callback/name pointer remains valid until exit. */
static struct {
    uc_controller_override rule;
    const pad_profile *profile;
} active[UC_OVERRIDE_MAX];
static size_t active_count;

void uc_controller_overrides_clear(void)
{
    memset(active, 0, sizeof(active));
    active_count = 0;
}
static int invalid(char *error, size_t cap, const char *message)
{
    if (error && cap)
        snprintf(error, cap, "%s", message);
    return -1;
}
static int validate(const uc_controller_overrides *rules, char *error, size_t cap)
{
    for (size_t i = 0; i < rules->count; i++) {
        const uc_controller_override *r = &rules->devices[i];
        const pad_profile *profile = NULL;
        if (r->transport == PAD_TRANSPORT_USB) {
            if (r->has_mapping)
                return invalid(error, cap, "USB HID mapping is not supported");
            if (r->has_profile && strcmp(r->profile, "xinput-usb"))
                return invalid(error, cap, "unknown USB profile");
        } else {
            if (r->has_profile && strcmp(r->profile, "generic-hid")) {
                profile = profile_builtin_named(r->profile);
                if (!profile)
                    return invalid(error, cap, "unknown Bluetooth profile");
            } else if (!r->has_profile) {
                profile = r->transport == PAD_TRANSPORT_CLASSIC
                              ? profile_classic_default(r->vid, r->pid, NULL, NULL)
                              : profile_find(r->vid, r->pid);
                if (r->has_mapping && r->transport == PAD_TRANSPORT_CLASSIC &&
                    profile_classic_unsupported(r->vid, r->pid))
                    return invalid(error, cap,
                                   "raw HID mapping requires explicit profile=generic-hid");
            }
            if (r->has_mapping && profile)
                return invalid(error, cap, "raw HID mapping requires explicit profile=generic-hid");
        }
    }
    return 0;
}
int uc_controller_overrides_load(const char *path, char *error, size_t capacity)
{
    uc_controller_overrides_clear();
    if (error && capacity)
        error[0] = 0;
    if (!path || !*path)
        return invalid(error, capacity, "invalid override path");
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? 0 : invalid(error, capacity, "override file cannot be opened");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > UC_OVERRIDE_FILE_MAX) {
        close(fd);
        return invalid(error, capacity, "override file must be a regular file of 1..32768 bytes");
    }
    unsigned char *bytes = malloc(UC_OVERRIDE_FILE_MAX + 1);
    uc_controller_overrides *rules = calloc(1, sizeof(*rules));
    if (!bytes || !rules) {
        close(fd);
        free(bytes);
        free(rules);
        return invalid(error, capacity, "override allocation failed");
    }
    size_t size = 0;
    int read_failed = 0;
    while (size <= UC_OVERRIDE_FILE_MAX) {
        ssize_t n = read(fd, bytes + size, UC_OVERRIDE_FILE_MAX + 1 - size);
        if (n < 0) {
            read_failed = 1;
            break;
        }
        if (!n)
            break;
        size += (size_t)n;
    }
    close(fd);
    int result = -1;
    if (read_failed || size > UC_OVERRIDE_FILE_MAX)
        invalid(error, capacity, "override file read failed or exceeded limit");
    else if (uc_controller_json_parse(bytes, size, rules, error, capacity) == 0 &&
             validate(rules, error, capacity) == 0) {
        for (size_t i = 0; i < rules->count; i++) {
            active[i].rule = rules->devices[i];
            const uc_controller_override *r = &active[i].rule;
            if (r->transport != PAD_TRANSPORT_USB && r->has_profile &&
                strcmp(r->profile, "generic-hid"))
                active[i].profile = profile_builtin_named(r->profile);
        }
        active_count = rules->count;
        result = 1;
    }
    free(bytes);
    free(rules);
    return result;
}
const uc_controller_override *uc_controller_override_find(pad_transport transport, uint16_t vid,
                                                          uint16_t pid)
{
    for (size_t i = 0; i < active_count; i++)
        if (active[i].rule.transport == transport && active[i].rule.has_ids &&
            !active[i].rule.has_address && !active[i].rule.has_match_name &&
            active[i].rule.vid == vid && active[i].rule.pid == pid)
            return &active[i].rule;
    return NULL;
}
const uc_controller_override *uc_controller_override_identity(pad_transport transport, uint16_t vid,
                                                              uint16_t pid,
                                                              const uint8_t address[6],
                                                              const char *name)
{
    const uc_controller_override *best = NULL;
    unsigned score = 0;
    for (size_t i = 0; i < active_count; i++) {
        const uc_controller_override *r = &active[i].rule;
        if (r->transport != transport || (r->has_ids && (r->vid != vid || r->pid != pid)) ||
            (r->has_address && (!address || memcmp(r->address, address, 6))) ||
            (r->has_match_name && (!name || strcmp(r->match_name, name))))
            continue;
        unsigned rank = r->has_address * 4 + r->has_match_name * 2 + r->has_ids;
        if (rank > score) {
            best = r;
            score = rank;
        }
    }
    return best;
}
int uc_controller_override_needs_name(uint16_t vid, uint16_t pid, const uint8_t address[6])
{
    for (size_t i = 0; i < active_count; i++) {
        const uc_controller_override *r = &active[i].rule;
        if (r->transport == PAD_TRANSPORT_CLASSIC && r->has_match_name &&
            (!r->has_ids || (r->vid == vid && r->pid == pid)) &&
            (!r->has_address || (address && !memcmp(r->address, address, 6))))
            return 1;
    }
    return 0;
}
int uc_controller_override_profile(pad_transport transport, uint16_t vid, uint16_t pid,
                                   const void **profile)
{
    if (!profile)
        return 0;
    for (size_t i = 0; i < active_count; i++) {
        const uc_controller_override *r = &active[i].rule;
        if (r->transport == transport && r->has_ids && !r->has_address && !r->has_match_name &&
            r->vid == vid && r->pid == pid && r->has_profile) {
            *profile = active[i].profile;
            return 1;
        }
    }
    return 0;
}
