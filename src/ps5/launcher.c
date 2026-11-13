#include "../logging.h"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "launcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>

#ifndef UC_LAUNCHER_ROOT
#define UC_LAUNCHER_ROOT "/user/app"
#endif
#ifndef UC_LAUNCHER_STAMP
#define UC_LAUNCHER_STAMP "/data/unicontroller/launcher.registered"
#endif
#ifndef UC_LAUNCHER_DISABLED
#define UC_LAUNCHER_DISABLED "/data/unicontroller/launcher.disabled"
#endif
static const char *state = "unsupported";
const char *uc_launcher_state(void)
{
    return state;
}
static int directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0755) && errno != EEXIST)
        return -1;
    /* Do not follow a substituted application-directory symlink. */
    if (lstat(path, &st) || !S_ISDIR(st.st_mode))
        return -1;
    return chmod(path, 0755); /* Payload uses umask 077; Shell must read the icon. */
}
static int same_file(const char *path, const unsigned char *data, size_t size)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != size)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    unsigned char buf[1024];
    size_t offset = 0;
    int same = 1;
    while (offset < size) {
        size_t n = size - offset < sizeof(buf) ? size - offset : sizeof(buf);
        if (fread(buf, 1, n, f) != n || memcmp(buf, data + offset, n)) {
            same = 0;
            break;
        }
        offset += n;
    }
    if (fclose(f))
        same = 0;
    return same;
}
static int put_file(const char *path, const unsigned char *data, size_t size)
{
    char temporary[512];
    int n = snprintf(temporary, sizeof temporary, "%s.tmpXXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof temporary)
        return -1;
    int fd = mkstemp(temporary);
    if (fd < 0)
        return -1;
    size_t offset = 0;
    int failed = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            failed = 1;
            break;
        }
        offset += (size_t)written;
    }
    if (fchmod(fd, 0644) || fsync(fd))
        failed = 1;
    if (close(fd))
        failed = 1;
    if (!failed && rename(temporary, path) == 0)
        return 0;
    (void)unlink(temporary);
    return -1;
}
static int resources(const char *root, unsigned short port, const unsigned char *icon, size_t size,
                     int write_files)
{
    char dir[384], sys[416], param_path[448], icon_path[448], param[512];
    if (!root || !icon || !size || (port != 7456 && port != 7457))
        return -1;
    int n = snprintf(dir, sizeof dir, "%s/%s", root, UC_LAUNCHER_TITLE_ID);
    if (n < 0 || (size_t)n >= sizeof dir)
        return -1;
    snprintf(sys, sizeof sys, "%s/sce_sys", dir);
    snprintf(param_path, sizeof param_path, "%s/param.json", sys);
    snprintf(icon_path, sizeof icon_path, "%s/icon0.png", sys);
    n = snprintf(param, sizeof param,
                 "{\"titleId\":\"%s\",\"applicationCategoryType\":65536,"
                 "\"deeplinkUri\":\"http://127.0.0.1:%u/\",\"localizedParameters\":{"
                 "\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"UniController\"}}}\n",
                 UC_LAUNCHER_TITLE_ID, (unsigned)port);
    if (n < 0 || (size_t)n >= sizeof param || (write_files && (directory(dir) || directory(sys))))
        return -1;
    int changed = 0;
    if (!same_file(param_path, (const unsigned char *)param, (size_t)n)) {
        if (write_files && put_file(param_path, (const unsigned char *)param, (size_t)n))
            return -1;
        changed = 1;
    }
    if (!same_file(icon_path, icon, size)) {
        if (write_files && put_file(icon_path, icon, size))
            return -1;
        changed = 1;
    }
    if (write_files && (chmod(param_path, 0644) || chmod(icon_path, 0644)))
        return -1;
    return changed;
}
int uc_launcher_write(const char *root, unsigned short port, const unsigned char *icon, size_t size)
{
    return resources(root, port, icon, size, 1);
}
#ifdef UC_PS5
#include "launcher_icon.inc"
static int pending, timing;
static unsigned short pending_port;
static uint64_t stable_since;
static int current_resources(unsigned short port)
{
    return same_file(UC_LAUNCHER_STAMP, (const unsigned char *)"1\n", 2) &&
           resources(UC_LAUNCHER_ROOT, port, uc_launcher_icon, sizeof uc_launcher_icon, 0) == 0;
}
void uc_launcher_startup(unsigned short port)
{
    pending = timing = 0;
    if (access(UC_LAUNCHER_DISABLED, F_OK) == 0)
        state = "removed";
    else if (current_resources(port))
        state = "registered";
    else {
        pending = 1;
        pending_port = port;
        state = "pending";
    }
    uc_log_info("[launcher] startup state=%s port=%u", state, (unsigned)port);
}
int uc_launcher_pending(void)
{
    return pending;
}
void uc_launcher_cancel(void)
{
    if (pending) {
        pending = timing = 0;
        state = "deferred";
        uc_log_info("[launcher] startup install cancelled; manual retry or next load required");
    }
}
void uc_launcher_poll(uint64_t now, int ready, int idle)
{
    if (!pending)
        return;
    if (!ready || !idle) {
        timing = 0;
        return;
    }
    if (!timing || now < stable_since) {
        stable_since = now;
        timing = 1;
        return;
    }
    if (now - stable_since < 5000)
        return;
    pending = timing = 0; /* One attempt, even if registration fails. */
    if (uc_launcher_install(pending_port, 0))
        uc_log_error("[launcher] automatic registration failed; no automatic retry");
}

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);
int kernel_dynlib_handle(pid_t, const char *, uint32_t *);
intptr_t kernel_dynlib_resolve(pid_t, uint32_t, const char *);

int uc_launcher_install(unsigned short port, int force)
{
    pending = timing = 0;
    if (!force && access(UC_LAUNCHER_DISABLED, F_OK) == 0) {
        state = "removed";
        return 0;
    }
    if (!force && current_resources(port)) {
        state = "registered";
        return 0;
    }
    state = "failed";
    /* Clear the success marker before modifying files, including partial updates. */
    int stamped = access(UC_LAUNCHER_STAMP, F_OK) == 0;
    if (unlink(UC_LAUNCHER_STAMP) && errno != ENOENT)
        return -1;
    int changed =
        uc_launcher_write(UC_LAUNCHER_ROOT, port, uc_launcher_icon, sizeof uc_launcher_icon);
    if (changed < 0) {
        uc_log_error("[launcher] resource_write_failed errno=%d", errno);
        return -1;
    }
    int rc = 0;
    if (changed || force || !stamped) {
        rc = sceAppInstUtilInitialize();
        if (!rc) {
            uint32_t handle;
            int (*install)(const char *, const char *, void *) = NULL;
            if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
                install = (int (*)(const char *, const char *, void *))kernel_dynlib_resolve(
                    -1, handle, "Wudg3Xe3heE");
            uc_log_info("[launcher] register method=%s title=%s port=%u",
                        install ? "TitleDir" : "InstallAll", UC_LAUNCHER_TITLE_ID, (unsigned)port);
            char base[384];
            snprintf(base, sizeof base, "%s/", UC_LAUNCHER_ROOT);
            rc = install ? install(UC_LAUNCHER_TITLE_ID, base, NULL)
                         : sceAppInstUtilAppInstallAll(NULL);
            (void)sceAppInstUtilTerminate();
        }
        uc_log_at(rc ? 1 : 3, "[launcher] registration_result=0x%08x", (unsigned)rc);
        if (rc && (uint32_t)rc != UINT32_C(0x80990002))
            return -1;
    }
    if (put_file(UC_LAUNCHER_STAMP, (const unsigned char *)"1\n", 2))
        return -1;
    if (unlink(UC_LAUNCHER_DISABLED) && errno != ENOENT)
        return -1;
    state = "registered";
    return 0;
}
int uc_launcher_remove(void)
{
    uc_launcher_cancel();
    int rc = sceAppInstUtilInitialize();
    if (!rc) {
        rc = sceAppInstUtilAppUnInstall(UC_LAUNCHER_TITLE_ID);
        (void)sceAppInstUtilTerminate();
    }
    uc_log_at(rc ? 1 : 3, "[launcher] uninstall_result=0x%08x", (unsigned)rc);
    if (rc)
        return -1; /* Keep files if system removal failed. */
    if (put_file(UC_LAUNCHER_DISABLED, (const unsigned char *)"1\n", 2))
        return -1;
    (void)unlink(UC_LAUNCHER_STAMP);
    char path[512];
    snprintf(path, sizeof path, "%s/%s/sce_sys/param.json", UC_LAUNCHER_ROOT, UC_LAUNCHER_TITLE_ID);
    (void)unlink(path);
    snprintf(path, sizeof path, "%s/%s/sce_sys/icon0.png", UC_LAUNCHER_ROOT, UC_LAUNCHER_TITLE_ID);
    (void)unlink(path);
    snprintf(path, sizeof path, "%s/%s/sce_sys", UC_LAUNCHER_ROOT, UC_LAUNCHER_TITLE_ID);
    (void)rmdir(path);
    snprintf(path, sizeof path, "%s/%s", UC_LAUNCHER_ROOT, UC_LAUNCHER_TITLE_ID);
    (void)rmdir(path);
    state = "removed";
    return 0;
}
#else
void uc_launcher_startup(unsigned short port)
{
    (void)port;
}
int uc_launcher_pending(void)
{
    return 0;
}
void uc_launcher_cancel(void)
{
}
void uc_launcher_poll(uint64_t now, int ready, int idle)
{
    (void)now;
    (void)ready;
    (void)idle;
}
int uc_launcher_install(unsigned short port, int force)
{
    (void)port;
    (void)force;
    return -1;
}
int uc_launcher_remove(void)
{
    return -1;
}
#endif
