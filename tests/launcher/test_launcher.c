/* Test filesystem resources and PS5 installer seams without touching /user/app. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define UC_PS5
#define UC_LAUNCHER_ROOT root
#define UC_LAUNCHER_STAMP stamp_path
#define UC_LAUNCHER_DISABLED disabled_path
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static char root[256], stamp_path[300], disabled_path[300];
#include "../../src/ps5/launcher.c"
static int initialized, terminated, title_calls, all_calls, init_error, install_error, use_title;
int sceAppInstUtilInitialize(void)
{
    initialized++;
    return init_error;
}
int sceAppInstUtilTerminate(void)
{
    terminated++;
    return 0;
}
int sceAppInstUtilAppInstallAll(void *p)
{
    assert(!p);
    all_calls++;
    return install_error;
}
int sceAppInstUtilAppUnInstall(const char *id)
{
    assert(!strcmp(id, UC_LAUNCHER_TITLE_ID));
    return install_error;
}
static int install_title(const char *id, const char *path, void *p)
{
    assert(!strcmp(id, UC_LAUNCHER_TITLE_ID));
    assert(!strncmp(path, root, strlen(root)) && path[strlen(root)] == '/');
    assert(!p);
    title_calls++;
    return install_error;
}
int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle)
{
    assert(pid == -1);
    assert(!strcmp(name, "libSceAppInstUtil.sprx"));
    *handle = 7;
    return 0;
}
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid)
{
    assert(pid == -1 && handle == 7);
    assert(!strcmp(nid, "Wudg3Xe3heE"));
    return use_title ? (intptr_t)install_title : 0;
}
int main(void)
{
    char temp[] = "/tmp/uc-launcher-XXXXXX";
    assert(mkdtemp(temp));
    snprintf(root, sizeof root, "%s", temp);
    snprintf(stamp_path, sizeof stamp_path, "%s/stamp", root);
    snprintf(disabled_path, sizeof disabled_path, "%s/disabled", root);
    const unsigned char icon[] = {1, 2, 3, 4};
    (void)umask(077);
    assert(uc_launcher_write(root, 7456, icon, sizeof icon) == 1);
    assert(uc_launcher_write(root, 7456, icon, sizeof icon) == 0);
    assert(uc_launcher_write(root, 7457, icon, sizeof icon) == 1);
    assert(uc_launcher_write(root, 7458, icon, sizeof icon) == -1);
    char path[512];
    snprintf(path, sizeof path, "%s/%s/sce_sys/param.json", root, UC_LAUNCHER_TITLE_ID);
    FILE *f = fopen(path, "rb");
    assert(f);
    char data[512] = {0};
    assert(fread(data, 1, sizeof data - 1, f) > 0);
    fclose(f);
    assert(strstr(data, "127.0.0.1:7457/") && strstr(data, "UniController"));
    struct stat st;
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0644);
    assert(!unlink(path));
    snprintf(path, sizeof path, "%s/%s/sce_sys/icon0.png", root, UC_LAUNCHER_TITLE_ID);
    assert(!unlink(path));
    snprintf(path, sizeof path, "%s/%s/sce_sys", root, UC_LAUNCHER_TITLE_ID);
    assert(!rmdir(path));
    snprintf(path, sizeof path, "%s/%s", root, UC_LAUNCHER_TITLE_ID);
    assert(!rmdir(path));
    use_title = 1;
    assert(uc_launcher_install(7456, 0) == 0 && title_calls == 1 && initialized == 1 &&
           terminated == 1);
    assert(uc_launcher_install(7456, 0) == 0 && title_calls == 1);
    assert(uc_launcher_install(7457, 0) == 0 && title_calls == 2);
    use_title = 0;
    assert(uc_launcher_install(7457, 1) == 0 && all_calls == 1);
    install_error = -123;
    assert(uc_launcher_install(7457, 1) == -1 && access(stamp_path, F_OK) != 0);
    assert(uc_launcher_remove() == -1);
    snprintf(path, sizeof path, "%s/%s/sce_sys/icon0.png", root, UC_LAUNCHER_TITLE_ID);
    assert(access(path, F_OK) == 0);
    install_error = 0;
    assert(uc_launcher_install(7457, 0) == 0 && all_calls == 3);
    init_error = -1;
    assert(uc_launcher_install(7457, 1) == -1 && access(stamp_path, F_OK) != 0);
    init_error = 0;
    assert(uc_launcher_remove() == 0 && !strcmp(uc_launcher_state(), "removed"));
    int calls = all_calls;
    assert(uc_launcher_install(7456, 0) == 0 && all_calls == calls);
    uc_launcher_startup(7456);
    assert(!uc_launcher_pending() && !strcmp(uc_launcher_state(), "removed"));
    assert(!unlink(disabled_path));
    /* Startup only schedules. Unknown power, logout and input activity reset the delay. */
    calls = all_calls;
    uc_launcher_startup(7456);
    assert(uc_launcher_pending() && all_calls == calls);
    assert(access(stamp_path, F_OK) != 0);
    uc_launcher_poll(1000, 0, 1);
    uc_launcher_poll(2000, 1, 1);
    uc_launcher_poll(6999, 1, 1);
    assert(all_calls == calls);
    uc_launcher_poll(7000, 1, 0);
    uc_launcher_poll(8000, 1, 1);
    uc_launcher_poll(12999, 1, 1);
    assert(all_calls == calls);
    uc_launcher_poll(13000, 1, 1);
    assert(all_calls == calls + 1 && !uc_launcher_pending());
    uc_launcher_poll(99999, 1, 1);
    assert(all_calls == calls + 1);
    /* Unchanged reload is read-only; rest/resume cannot queue an update. */
    struct stat before, after;
    assert(!stat(stamp_path, &before));
    uc_launcher_startup(7456);
    assert(!uc_launcher_pending() && !stat(stamp_path, &after));
    assert(before.st_ino == after.st_ino);
    uc_launcher_startup(7457);
    assert(uc_launcher_pending());
    uc_launcher_cancel();
    uc_launcher_poll(200000, 1, 1);
    assert(!uc_launcher_pending() && all_calls == calls + 1);
    /* Failure is attempted once, only another startup/manual operation retries. */
    uc_launcher_startup(7457);
    install_error = -123;
    uc_launcher_poll(300000, 1, 1);
    uc_launcher_poll(305000, 1, 1);
    uc_launcher_poll(400000, 1, 1);
    assert(all_calls == calls + 2 && !uc_launcher_pending());
    install_error = 0;
    uc_launcher_startup(7457);
    assert(uc_launcher_pending());
    assert(uc_launcher_install(7457, 1) == 0 && !uc_launcher_pending());
    assert(uc_launcher_remove() == 0);
    assert(!unlink(disabled_path));
    snprintf(path, sizeof path, "%s/%s", root, UC_LAUNCHER_TITLE_ID);
    assert(!symlink(root, path));
    assert(uc_launcher_write(root, 7456, icon, sizeof icon) == -1);
    assert(!unlink(path));
    assert(!rmdir(root));
    puts("Launcher resource checks passed");
}
