#include "../logging.h"
#include "../drivers/config/overrides.h"
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "json.h"
#include "assets.h"
#include "../drivers/bt.h"
#include "../drivers/usb.h"
#include "../ps5/launcher.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <strings.h>
#include <signal.h>

#define CLIENTS 8
#define REQUEST_MAX 12288
#define HEADER_MAX 4096
#define BODY_MAX 8192
#define RESPONSE_MAX 131072
__attribute__((weak)) const char uc_index_html[] =
    "<!doctype html><title>UniController</title><p>UI resources unavailable.</p>";
__attribute__((weak)) const char uc_controller_html[] =
    "<!doctype html><title>Controller</title><p>UI resources unavailable.</p>";
typedef struct {
    int fd;
    int stop_after_send;
    size_t used, sent, total;
    uint64_t deadline;
    char in[REQUEST_MAX + 1];
    char out[RESPONSE_MAX];
} Client;
typedef struct {
    int recording, page, rumble, swap, bt_auto, bt_auto_resume;
    int32_t bt_auto_user;
    uint8_t map[32];
} Settings;
__attribute__((weak)) const char uc_settings_html[] =
    "<!doctype html><title>Settings</title><p>UI resources unavailable.</p>";
static Client clients[CLIENTS];
static Settings settings;
static char token[65], origin[160];
static unsigned short listening_port;
static const char *store_path;
static uint64_t active_session;
static int active_user = -1;
static int usb_start_queued;
static int32_t usb_start_user;
static uint64_t usb_start_at;
static int bt_start_queued;
static int32_t bt_start_user;
static uint64_t bt_start_at;
static int enable_pending;
static volatile sig_atomic_t stop_requested;
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}
static const char *levels[] = {"off", "error", "warn", "info", "debug", "trace"};
static uint64_t millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int nonblock(int fd)
{
    int f = fcntl(fd, F_GETFL, 0);
    return f < 0 ? -1 : fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
static void close_client(Client *c)
{
    close(c->fd);
    c->fd = -1;
}
static void response_bytes(Client *c, int status, const char *type, const char *body, size_t len)
{
    const char *reason = status == 200   ? "OK"
                         : status == 202 ? "Accepted"
                         : status == 400 ? "Bad Request"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 413 ? "Payload Too Large"
                         : status == 409 ? "Conflict"
                                         : "Service Unavailable";
    int n = snprintf(
        c->out, sizeof(c->out),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: "
        "close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: "
        "no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src "
        "'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n\r\n",
        status, reason, type, len);
    if (n < 0 || (size_t)n + len >= sizeof(c->out)) {
        close_client(c);
        return;
    }
    memcpy(c->out + n, body, len);
    c->total = (size_t)n + len;
    c->sent = 0;
}
static void response(Client *c, int status, const char *type, const char *body)
{
    response_bytes(c, status, type, body, strlen(body));
}
static void error_response(Client *c, int code, const char *message)
{
    char b[128];
    snprintf(b, sizeof(b), "{\"error\":\"%s\"}", message);
    response(c, code, "application/json", b);
}
static int level(Json *j, int n)
{
    for (int i = 0; i < 6; i++)
        if (n >= 0 && jeq(j, n, levels[i]))
            return i;
    return -1;
}
static int array(Json *j, int n, int64_t min, int64_t max, int count, int64_t *out)
{
    if (n < 0 || j->t[n].kind != '[')
        return -1;
    int at = n + 1;
    for (int k = 0; k < count; k++) {
        if (at >= j->t[n].next || jint(j, at, min, max, &out[k]))
            return -1;
        at = j->t[at].next;
    }
    return at == j->t[n].next ? 0 : -1;
}
static int parse_settings(const char *body, Settings *out)
{
    Json j;
    const char *keys[] = {"schema_version",
                          "recording_level",
                          "page_level",
                          "rumble",
                          "swap_ab",
                          "button_mapping",
                          "bluetooth_auto_start",
                          "bluetooth_auto_user",
                          "bluetooth_auto_trigger"};
    if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 9))
        return -1;
    int64_t version, map[32];
    if (jint(&j, jget(&j, "schema_version"), 1, 1, &version) ||
        array(&j, jget(&j, "button_mapping"), 0, 31, 32, map))
        return -1;
    out->bt_auto_resume = 0;
    int trigger = jget(&j, "bluetooth_auto_trigger");
    if (trigger >= 0) {
        if (jeq(&j, trigger, "resume"))
            out->bt_auto_resume = 1;
        else if (!jeq(&j, trigger, "login"))
            return -1;
    }
    out->bt_auto = 0;
    out->bt_auto_user = 0;
    int mode = jget(&j, "bluetooth_auto_start");
    if (mode >= 0 && jbool(&j, mode, &out->bt_auto))
        return -1;
    int user = jget(&j, "bluetooth_auto_user");
    int64_t user_id = 0;
    if (user >= 0 && jint(&j, user, 0, INT32_MAX, &user_id))
        return -1;
    out->bt_auto_user = (int32_t)user_id;
    out->recording = level(&j, jget(&j, "recording_level"));
    out->page = level(&j, jget(&j, "page_level"));
    if (out->recording < 0 || out->page < 0 || jbool(&j, jget(&j, "rumble"), &out->rumble) ||
        jbool(&j, jget(&j, "swap_ab"), &out->swap))
        return -1;
    for (int i = 0; i < 32; i++)
        out->map[i] = (uint8_t)map[i];
    return 0;
}
static void settings_json(const Settings *s, char *out, size_t cap)
{
    int n = snprintf(
        out, cap,
        "{\"schema_version\":1,\"recording_level\":\"%s\",\"page_level\":\"%s\","
        "\"rumble\":%s,\"swap_ab\":%s,\"bluetooth_auto_start\":%s,"
        "\"bluetooth_auto_user\":%d,\"bluetooth_auto_trigger\":\"%s\",\"button_mapping\":[",
        levels[s->recording], levels[s->page], s->rumble ? "true" : "false",
        s->swap ? "true" : "false", s->bt_auto ? "true" : "false", s->bt_auto_user,
        s->bt_auto_resume ? "resume" : "login");
    for (int i = 0; i < 32; i++)
        n += snprintf(out + n, cap - (size_t)n, "%s%u", i ? "," : "", s->map[i]);
    snprintf(out + n, cap - (size_t)n, "]}");
}
static int save_settings(const Settings *s)
{
    char body[1024], temporary[1024];
    settings_json(s, body, sizeof(body));
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", store_path) >= (int)sizeof(temporary))
        return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    size_t len = strlen(body), at = 0;
    while (at < len) {
        ssize_t n = write(fd, body + at, len - at);
        if (n <= 0) {
            close(fd);
            unlink(temporary);
            return -1;
        }
        at += (size_t)n;
    }
    int ok = fsync(fd);
    if (close(fd))
        ok = -1;
    if (ok || rename(temporary, store_path)) {
        unlink(temporary);
        return -1;
    }
    return 0;
}
static int load_settings(void)
{
    settings.recording = 3;
    settings.page = 3;
    settings.rumble = 1;
    settings.swap = 0;
    settings.bt_auto = 0;
    settings.bt_auto_resume = 0;
    settings.bt_auto_user = 0;
    for (int i = 0; i < 32; i++)
        settings.map[i] = (uint8_t)i;
    int fd = open(store_path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    char body[2048];
    ssize_t len = read(fd, body, sizeof(body));
    close(fd);
    if (len <= 0 || len >= (ssize_t)sizeof(body))
        return -1;
    body[len] = 0;
    Settings parsed;
    if (parse_settings(body, &parsed))
        return -1;
    settings = parsed;
    return 0;
}
static int secret_equal(const char *a, const char *b)
{
    size_t n = strlen(a);
    if (n != strlen(b))
        return 0;
    unsigned x = 0;
    for (size_t i = 0; i < n; i++)
        x |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return x == 0;
}
static int make_token(void)
{
    unsigned char bytes[32];
#ifdef UC_PS5
    extern void arc4random_buf(void *, size_t);
    arc4random_buf(bytes, sizeof(bytes));
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t at = 0;
    while (at < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + at, sizeof(bytes) - at);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        at += (size_t)n;
    }
    close(fd);
#endif
    for (int i = 0; i < 32; i++)
        snprintf(token + i * 2, 3, "%02x", bytes[i]);
    return 0;
}
static void handle(Client *c, char *method, char *path, char *host, char *request_origin,
                   char *auth, char *body)
{
    char expected[160];
    if (!host || snprintf(expected, sizeof(expected), "http://%s", host) >= (int)sizeof(expected)) {
        error_response(c, 400, "invalid_host");
        return;
    }
    /* Bind origin to configured address when explicit; wildcard bind uses the Host authority. */
    const char *allowed = origin[0] ? origin : expected;
    int mutation = !strcmp(method, "POST") || !strcmp(method, "DELETE");
    if (mutation && (!auth || !secret_equal(auth, token) || !request_origin ||
                     strcmp(request_origin, allowed))) {
        error_response(c, 403, "forbidden");
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/session")) {
        if (request_origin && strcmp(request_origin, allowed)) {
            error_response(c, 403, "origin_denied");
            return;
        }
        char b[100];
        snprintf(b, sizeof(b), "{\"token\":\"%s\"}", token);
        response(c, 200, "application/json", b);
        return;
    }
    if (!strcmp(method, "GET") &&
        (!strcmp(path, "/") || !strcmp(path, "/controller") || !strcmp(path, "/settings"))) {
        response(c, 200, "text/html; charset=utf-8",
                 !strcmp(path, "/")           ? uc_index_html
                 : !strcmp(path, "/settings") ? uc_settings_html
                                              : uc_controller_html);
        return;
    }
    if (!strcmp(method, "GET")) {
        for (size_t i = 0; i < uc_assets_count; i++)
            if (!strcmp(path, uc_assets[i].path)) {
                response_bytes(c, 200, uc_assets[i].type, uc_assets[i].body, uc_assets[i].size);
                return;
            }
    }
    if (!strcmp(path, "/api/v1/launcher")) {
        if (strcmp(method, "GET")) {
            /* Installer calls can block; perform maintenance only with inputs stopped. */
            if (uc_bt_running() || uc_usb_running() || bt_start_queued || usb_start_queued ||
                uc_core_active_session() || enable_pending) {
                error_response(c, 409, "disable_inputs_before_launcher_change");
                return;
            }
            int rc;
            if (!strcmp(method, "POST"))
                rc = uc_launcher_install(listening_port, 1);
            else if (!strcmp(method, "DELETE"))
                rc = uc_launcher_remove();
            else {
                error_response(c, 404, "not_found");
                return;
            }
            if (rc) {
                error_response(c, 503, "launcher_operation_failed_or_unsupported");
                return;
            }
        }
        char status[192];
        snprintf(status, sizeof status, "{\"state\":\"%s\",\"title_id\":\"%s\",\"port\":%u}",
                 uc_launcher_state(), UC_LAUNCHER_TITLE_ID, (unsigned)listening_port);
        response(c, 200, "application/json", status);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/bluetooth/diagnostics")) {
        char *status = malloc(RESPONSE_MAX - 1024);
        if (!status || !uc_bt_diagnostics_json(status, RESPONSE_MAX - 1024))
            error_response(c, 503, "bluetooth_diagnostics_unavailable");
        else
            response(c, 200, "application/json", status);
        free(status);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/usb/diagnostics")) {
        char *status = malloc(RESPONSE_MAX - 1024);
        if (!status || !uc_usb_diagnostics_json(status, RESPONSE_MAX - 1024))
            error_response(c, 503, "usb_diagnostics_unavailable");
        else
            response(c, 200, "application/json", status);
        free(status);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/usb")) {
        if (usb_start_queued) {
            response(c, 200, "application/json",
                     "{\"state\":\"starting\",\"reason\":\"\",\"devices\":[],"
                     "\"pending\":true,\"bound\":false}");
            return;
        }
        char status[32768];
        if (!uc_usb_status_json(status, sizeof(status)))
            error_response(c, 503, "usb_status_overflow");
        else
            response(c, 200, "application/json", status);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/usb/start")) {
        Json j;
        int64_t user;
        const char *keys[] = {"user_id"};
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 1) ||
            jint(&j, jget(&j, "user_id"), 1, INT32_MAX, &user)) {
            error_response(c, 400, "invalid_user");
            return;
        }
        if (usb_start_queued || uc_usb_running() || active_session || enable_pending) {
            error_response(c, 409, "disable_existing_source_first");
            return;
        }
        if (!uc_backend_can_start((int32_t)user)) {
            error_response(c, 409, "signed_in_user_and_stable_awake_required");
            return;
        }
        usb_start_user = (int32_t)user;
        usb_start_queued = 1;
        usb_start_at = millis() + 50;
        response(c, 202, "application/json", "{\"pending\":true}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/usb/stop")) {
        usb_start_queued = 0;
        int rc = uc_usb_stop();
        response(c, rc ? 202 : 200, "application/json",
                 rc ? "{\"pending\":true}" : "{\"stopped\":true}");
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/bluetooth")) {
        if (bt_start_queued) {
            response(c, 200, "application/json",
                     "{\"state\":\"starting\",\"reason\":\"\",\"devices\":[],\"paired\":[],"
                     "\"scan_seconds\":0,\"scan_id\":0,\"discovered\":[]}");
            return;
        }
        char body_json[32768];
        if (!uc_bt_status_json(body_json, sizeof(body_json)))
            error_response(c, 503, "bluetooth_status_overflow");
        else
            response(c, 200, "application/json", body_json);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/bluetooth/start")) {
        Json j;
        int64_t user;
        const char *keys[] = {"user_id"};
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 1) ||
            jint(&j, jget(&j, "user_id"), 1, INT32_MAX, &user)) {
            error_response(c, 400, "invalid_user");
            return;
        }
        if (bt_start_queued || uc_bt_running() || active_session || enable_pending) {
            error_response(c, 409, "disable_existing_source_first");
            return;
        }
        if (!uc_backend_can_start((int32_t)user)) {
            error_response(c, 409, "signed_in_user_and_stable_awake_required");
            return;
        }
        uc_backend_auto_watch(0);
        bt_start_user = (int32_t)user;
        bt_start_queued = 1;
        bt_start_at = millis() + 50;
        response(c, 202, "application/json", "{\"pending\":true}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/bluetooth/stop")) {
        uc_backend_auto_watch(0);
        bt_start_queued = 0;
        int rc = uc_bt_stop();
        response(c, rc ? 202 : 200, "application/json",
                 rc ? "{\"pending\":true}" : "{\"stopped\":true}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/bluetooth/scan")) {
        Json j;
        int64_t seconds;
        const char *keys[] = {"seconds"};
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 1) ||
            jint(&j, jget(&j, "seconds"), 1, 120, &seconds)) {
            error_response(c, 400, "invalid_scan_window");
            return;
        }
        if (uc_bt_scan((unsigned)seconds))
            error_response(c, 409, "bluetooth_not_ready_or_pairing");
        else
            response(c, 200, "application/json", "{\"scanning\":true}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/bluetooth/scan/stop")) {
        if (uc_bt_scan(0))
            error_response(c, 409, "bluetooth_not_ready");
        else
            response(c, 200, "application/json", "{\"scanning\":false}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/bluetooth/pair")) {
        Json j;
        int64_t id;
        const char *keys[] = {"discovery_id"};
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 1) ||
            jint(&j, jget(&j, "discovery_id"), 1, UINT32_MAX, &id)) {
            error_response(c, 400, "invalid_discovery_id");
            return;
        }
        if (uc_bt_pair((uint32_t)id))
            error_response(c, 409, "device_not_available_or_pairing");
        else
            response(c, 202, "application/json", "{\"pairing\":true}");
        return;
    }
    if (!strcmp(method, "POST") &&
        (!strcmp(path, "/api/v1/bluetooth/assign") || !strcmp(path, "/api/v1/usb/assign"))) {
        int wireless = !strcmp(path, "/api/v1/bluetooth/assign");
        Json j;
        int64_t slot, user;
        const char *identity = wireless ? "address" : "node";
        const char *keys[] = {"slot", identity, "user_id"};
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 3) ||
            jint(&j, jget(&j, "slot"), 0, 3, &slot) ||
            jint(&j, jget(&j, "user_id"), 1, INT32_MAX, &user)) {
            error_response(c, 400, "invalid_assignment");
            return;
        }
        int n = jget(&j, identity);
        char device[128];
        int len = n >= 0 ? j.t[n].end - j.t[n].start - 2 : 0;
        if (n < 0 || j.t[n].kind != '"' || len <= 0 || len >= (int)sizeof(device)) {
            error_response(c, 400, "invalid_device_identity");
            return;
        }
        memcpy(device, j.s + j.t[n].start + 1, (size_t)len);
        device[len] = 0;
        /* Device identities are literal ASCII, never escaped JSON strings. */
        if (strchr(device, '\\') || !uc_backend_can_start((int32_t)user)) {
            error_response(c, 409, "signed_in_user_and_stable_awake_required");
            return;
        }
        int rc = wireless ? uc_bt_assign((int)slot, device, (int32_t)user)
                          : uc_usb_assign((unsigned)slot, device, (int32_t)user);
        if (rc < 0)
            error_response(c, 409, "device_or_user_unavailable");
        else
            response(c, rc == 2 ? 202 : 200, "application/json",
                     rc == 2 ? "{\"pending\":true}" : "{\"assigned\":true}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/bluetooth/forget")) {
        Json j;
        const char *keys[] = {"address"};
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 1)) {
            error_response(c, 400, "invalid_address");
            return;
        }
        int n = jget(&j, "address");
        if (n < 0 || j.t[n].kind != '"' || j.t[n].end - j.t[n].start != 19) {
            error_response(c, 400, "invalid_address");
            return;
        }
        char address[18];
        memcpy(address, j.s + j.t[n].start + 1, 17);
        address[17] = 0;
        if (uc_bt_forget(address))
            error_response(c, 409, "forget_failed_or_not_ready");
        else
            response(c, 200, "application/json", "{\"forgotten\":true}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/stop")) {
        response(c, 200, "application/json", "{\"stopping\":true}");
        c->stop_after_send = 1;
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/diagnostics")) {
        FILE *file = fopen("/data/unicontroller/runtime.log", "rb");
        if (!file) {
            error_response(c, 404, "runtime_log_unavailable");
            return;
        }
        char text[32769];
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            error_response(c, 503, "log_read_failed");
            return;
        }
        long size = ftell(file);
        if (size < 0 || fseek(file, size > 32768 ? size - 32768 : 0, SEEK_SET) != 0) {
            fclose(file);
            error_response(c, 503, "log_read_failed");
            return;
        }
        size_t count = fread(text, 1, sizeof(text) - 1, file);
        int failed = ferror(file);
        fclose(file);
        if (failed) {
            error_response(c, 503, "log_read_failed");
            return;
        }
        text[count] = 0;
        response(c, 200, "text/plain; charset=utf-8", text);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/system")) {
#ifdef UC_PS5
        response(c, 200, "application/json",
                 "{\"version\":\"0.1.0\",\"platform\":\"ps5\",\"adapter_ready\":true}");
#else
        response(c, 200, "application/json",
                 "{\"version\":\"0.1.0\",\"platform\":\"host-mock\",\"adapter_ready\":true}");
#endif
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/v1/users")) {
        int32_t ids[4] = {-1, -1, -1, -1};
        if (uc_backend_users(ids)) {
            error_response(c, 503, "users_unavailable");
            return;
        }
        char b[512];
        int n = snprintf(b, sizeof(b), "{\"users\":[");
        int count = 0;
        for (int i = 0; i < 4; i++)
            if (ids[i] > 0) {
                n += snprintf(b + n, sizeof(b) - (size_t)n,
                              "%s{\"user_id\":%d,\"display_name\":\"User %d\"}", count++ ? "," : "",
                              ids[i], ids[i]);
            }
        snprintf(b + n, sizeof(b) - (size_t)n, "],\"preferred_user_id\":%d}",
                 uc_backend_preferred_user());
        response(c, 200, "application/json", b);
        return;
    }
    if (!strcmp(path, "/api/v1/settings")) {
        if (!strcmp(method, "POST")) {
            if (active_session || enable_pending || uc_bt_running() || bt_start_queued ||
                uc_usb_running() || usb_start_queued) {
                error_response(c, 409, "disable_before_settings");
                return;
            }
            Settings next;
            if (parse_settings(body, &next)) {
                error_response(c, 400, "invalid_settings");
                return;
            }
            /* Account is learned from a successful manual Bluetooth start. */
            next.bt_auto_user = settings.bt_auto_user;
            if (save_settings(&next)) {
                if (settings.recording >= 1)
                    uc_log_error("settings_persist_failed");
                error_response(c, 503, "settings_persist_failed");
                return;
            }
            settings = next;
            uc_backend_auto_watch(settings.bt_auto && !settings.bt_auto_resume ? UC_AUTO_DESKTOP
                                                                               : UC_AUTO_OFF);
            if (uc_core_settings((uint8_t)settings.recording, (uint8_t)settings.swap,
                                 settings.map)) {
                if (settings.recording >= 1)
                    uc_log_error("settings_apply_failed");
                error_response(c, 503, "settings_apply_failed");
                return;
            }
            uc_bt_set_log_level((unsigned)settings.recording);
            uc_bt_set_rumble_enabled(settings.rumble);
            uc_usb_set_log_level((unsigned)settings.recording);
            uc_usb_set_rumble_enabled(settings.rumble);
            if (settings.recording >= 3)
                uc_log_info("settings_saved");
        } else if (strcmp(method, "GET")) {
            error_response(c, 404, "not_found");
            return;
        }
        char b[1024];
        settings_json(&settings, b, sizeof(b));
        response(c, 200, "application/json", b);
        return;
    }
    if (!strcmp(method, "GET") && !strncmp(path, "/api/v1/logs", 12) &&
        (path[12] == 0 || path[12] == '?')) {
        int filter = settings.page;
        if (path[12]) {
            if (strncmp(path + 12, "?level=", 7)) {
                error_response(c, 400, "invalid_query");
                return;
            }
            filter = -1;
            for (int i = 0; i < 6; i++)
                if (!strcmp(path + 19, levels[i]))
                    filter = i;
            if (filter < 0) {
                error_response(c, 400, "invalid_level");
                return;
            }
        }
        char b[32768];
        size_t len = uc_core_logs(b, sizeof(b), (uint8_t)filter);
        if (!len || len >= sizeof(b)) {
            error_response(c, 503, "logs_overflow");
            return;
        }
        b[len] = 0;
        response(c, 200, "application/json", b);
        return;
    }
    if (!strcmp(path, "/api/v1/test-controller")) {
        if (uc_usb_running() || usb_start_queued) {
            if (!strcmp(method, "GET"))
                response(c, 200, "application/json",
                         "{\"enabled\":false,\"session_id\":null,\"source\":\"usb\"}");
            else
                error_response(c, 409, "usb_owns_output");
            return;
        }
        if (uc_bt_running() || bt_start_queued) {
            if (!strcmp(method, "GET"))
                response(c, 200, "application/json",
                         "{\"enabled\":false,\"session_id\":null,\"source\":\"bluetooth\"}");
            else
                error_response(c, 409, "bluetooth_owns_output");
            return;
        }
        if (!strcmp(method, "POST")) {
            Json j;
            const char *keys[] = {"user_id"};
            int64_t user;
            if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 1) ||
                jint(&j, jget(&j, "user_id"), 1, INT32_MAX, &user)) {
                error_response(c, 400, "invalid_user");
                return;
            }
            if (active_session) {
                error_response(c, 409, "already_enabled");
                return;
            }
            uint64_t session = 0;
            int rc = uc_core_enable((int32_t)user, millis(), &session);
            if (rc == 2) {
                enable_pending = 1;
                response(c, 202, "application/json", "{\"pending\":true}");
                return;
            }
            if (rc || !session) {
                error_response(c, 503, "enable_failed");
                return;
            }
            enable_pending = 0;
            active_session = session;
            active_user = (int)user;
        } else if (!strcmp(method, "DELETE")) {
            if (uc_core_disable()) {
                error_response(c, 503, "disable_failed");
                return;
            }
            active_session = 0;
            active_user = -1;
            enable_pending = 0;
        } else if (strcmp(method, "GET")) {
            error_response(c, 404, "not_found");
            return;
        }
        char b[160];
        if (active_session)
            snprintf(b, sizeof(b), "{\"enabled\":true,\"session_id\":%llu,\"user_id\":%d}",
                     (unsigned long long)active_session, active_user);
        else
            snprintf(b, sizeof(b), "{\"enabled\":false,\"session_id\":null,\"pending\":%s}",
                     enable_pending ? "true" : "false");
        response(c, 200, "application/json", b);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/v1/test-controller/input")) {
        if (uc_usb_running() || usb_start_queued) {
            error_response(c, 409, "usb_owns_output");
            return;
        }
        if (uc_bt_running() || bt_start_queued) {
            error_response(c, 409, "bluetooth_owns_output");
            return;
        }
        Json j;
        const char *keys[] = {"session_id", "sequence", "buttons", "axes", "triggers"};
        int64_t session, seq, buttons, axes[4], triggers[2];
        if (jparse(&j, body, strlen(body)) || jobject(&j, keys, 5) ||
            jint(&j, jget(&j, "session_id"), 1, INT64_MAX, &session) ||
            jint(&j, jget(&j, "sequence"), 0, INT64_MAX, &seq) ||
            jint(&j, jget(&j, "buttons"), 0, UINT32_MAX, &buttons) ||
            array(&j, jget(&j, "axes"), INT16_MIN, INT16_MAX, 4, axes) ||
            array(&j, jget(&j, "triggers"), 0, UINT16_MAX, 2, triggers)) {
            error_response(c, 400, "invalid_input");
            return;
        }
        int16_t a[4];
        uint16_t t[2];
        for (int i = 0; i < 4; i++)
            a[i] = (int16_t)axes[i];
        for (int i = 0; i < 2; i++)
            t[i] = (uint16_t)triggers[i];
        if ((uint64_t)session != active_session ||
            uc_core_submit((uint64_t)session, (uint64_t)seq, (uint32_t)buttons, a, t, millis())) {
            error_response(c, 409, "input_rejected");
            return;
        }
        response(c, 200, "application/json", "{\"accepted\":true}");
        return;
    }
    error_response(c, 404, "not_found");
}
/* No keepalive, transfer encoding, folded headers or pipelining: one bounded request per socket. */
static void parse_request(Client *c)
{
    c->in[c->used] = 0;
    char *end = strstr(c->in, "\r\n\r\n");
    if (!end) {
        if (c->used >= HEADER_MAX)
            error_response(c, 413, "headers_too_large");
        return;
    }
    size_t header = (size_t)(end - c->in) + 4;
    if (header > HEADER_MAX) {
        error_response(c, 413, "headers_too_large");
        return;
    }
    char copy[HEADER_MAX + 1];
    memcpy(copy, c->in, header);
    copy[header] = 0;
    char *line = strstr(copy, "\r\n");
    if (!line) {
        error_response(c, 400, "invalid_request");
        return;
    }
    *line = 0;
    char method[12], path[512], version[16], extra;
    if (sscanf(copy, "%11s %511s %15s %c", method, path, version, &extra) != 3 ||
        strcmp(version, "HTTP/1.1") || strchr(path, '%') || strchr(path, '#') || path[0] != '/') {
        error_response(c, 400, "invalid_request");
        return;
    }
    char *host = NULL, *auth = NULL, *request_origin = NULL;
    size_t length = 0;
    int have_length = 0;
    char *p = line + 2;
    while (*p) {
        char *next = strstr(p, "\r\n");
        if (!next)
            break;
        *next = 0;
        if (!*p)
            break;
        if (*p == ' ' || *p == '\t') {
            error_response(c, 400, "folded_header");
            return;
        }
        char *colon = strchr(p, ':');
        if (!colon) {
            error_response(c, 400, "invalid_header");
            return;
        }
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        char *tail = v + strlen(v);
        while (tail > v && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        if (!strcasecmp(p, "Transfer-Encoding")) {
            error_response(c, 400, "transfer_encoding_unsupported");
            return;
        }
        if (!strcasecmp(p, "Content-Length")) {
            if (have_length || !*v) {
                error_response(c, 400, "invalid_length");
                return;
            }
            have_length = 1;
            for (char *q = v; *q; q++) {
                if (*q < '0' || *q > '9' || length > BODY_MAX) {
                    error_response(c, 413, "body_too_large");
                    return;
                }
                length = length * 10 + (size_t)(*q - '0');
            }
            if (length > BODY_MAX) {
                error_response(c, 413, "body_too_large");
                return;
            }
        }
        char **dest = !strcasecmp(p, "Host")         ? &host
                      : !strcasecmp(p, "Origin")     ? &request_origin
                      : !strcasecmp(p, "X-UC-Token") ? &auth
                                                     : NULL;
        if (dest) {
            if (*dest) {
                error_response(c, 400, "duplicate_header");
                return;
            }
            *dest = v;
        }
        p = next + 2;
    }
    if (c->used < header + length)
        return;
    if (c->used != header + length) {
        error_response(c, 400, "unexpected_bytes");
        return;
    }
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    char ip[INET_ADDRSTRLEN], authority[80];
    if (getsockname(c->fd, (struct sockaddr *)&local, &size) ||
        !inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip))) {
        error_response(c, 400, "invalid_host");
        return;
    }
    snprintf(authority, sizeof(authority), "%s:%u", ip, (unsigned)ntohs(local.sin_port));
    if (!host || strcmp(host, authority)) {
        error_response(c, 403, "host_denied");
        return;
    }
    c->in[header + length] = 0;
    if (memchr(c->in + header, 0, length)) {
        error_response(c, 400, "invalid_body");
        return;
    }
    handle(c, method, path, host, request_origin, auth, c->in + header);
}
int uc_runtime_run(const char *bind_address, unsigned short port, const char *settings_path)
{
    store_path = settings_path;
    if (!port || !settings_path || make_token() || load_settings())
        return -1;
    if (uc_core_settings((uint8_t)settings.recording, (uint8_t)settings.swap, settings.map))
        return -1;
    char override_path[512], override_error[160];
    const char *slash = strrchr(settings_path, '/');
    int length = slash ? (int)(slash - settings_path) : 1;
    int written = snprintf(override_path, sizeof override_path, "%.*s/controllers.json", length,
                           slash ? settings_path : ".");
    uc_controller_overrides_clear();
    if (written < 0 || (size_t)written >= sizeof override_path) {
        uc_log_warn("controller override path too long; using builtins");
    } else {
        int loaded =
            uc_controller_overrides_load(override_path, override_error, sizeof override_error);
        if (loaded < 0)
            uc_log_warn("controller overrides ignored: %s", override_error);
        else if (settings.recording >= 3)
            uc_log_info("controller registry: %s",
                        loaded ? "custom overrides loaded" : "builtins active");
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1)
        return -1;
    int listener = -1;
    unsigned short first_port = port;
    int attempts = port == 7456 ? 2 : 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        port = (unsigned short)(first_port + attempt);
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) {
            uc_log_error("HTTP socket failed port=%u errno=%d", (unsigned)port, errno);
            return -1;
        }
        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        address.sin_port = htons(port);
        if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0)
            break;
        int bind_error = errno;
        close(listener);
        listener = -1;
        uc_log_warn("HTTP bind failed port=%u errno=%d%s", (unsigned)port, bind_error,
                    attempt + 1 < attempts ? "; trying 7457" : "; no ports left");
    }
    if (listener < 0)
        return -1;
    if (listen(listener, CLIENTS) || nonblock(listener)) {
        uc_log_error("HTTP listen failed port=%u errno=%d", (unsigned)port, errno);
        close(listener);
        return -1;
    }
    uc_backend_auto_watch(settings.bt_auto && !settings.bt_auto_resume ? UC_AUTO_DESKTOP
                                                                       : UC_AUTO_OFF);
    listening_port = port;
#ifdef UC_PS5
    uc_launcher_startup(port);
#endif
    uc_bt_set_log_level((unsigned)settings.recording);
    uc_bt_set_rumble_enabled(settings.rumble);
    uc_usb_set_log_level((unsigned)settings.recording);
    uc_usb_set_rumble_enabled(settings.rumble);
    origin[0] = 0;
    for (int i = 0; i < CLIENTS; i++)
        clients[i].fd = -1;
#ifndef UC_PS5
    signal(SIGPIPE, SIG_IGN);
#endif
    uc_log_info("UniController HTTP listening on %s:%u (no TLS; trusted LAN only)", bind_address,
                (unsigned)port);
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    while (!stop_requested) {
        uc_backend_poll();
        if (uc_backend_suspend()) {
            uc_launcher_cancel();
            uc_backend_auto_watch(!settings.bt_auto         ? UC_AUTO_OFF
                                  : settings.bt_auto_resume ? UC_AUTO_RESUME
                                                            : UC_AUTO_DESKTOP);
            bt_start_queued = 0;
            usb_start_queued = 0;
            uc_bt_suspend();
            uc_usb_suspend();
            uc_core_suspend();
            active_session = 0;
            active_user = -1;
            enable_pending = 0;
        }
        if (uc_launcher_pending()) {
            int32_t users[4];
            int logged_in = 0;
            if (!uc_backend_users(users))
                for (unsigned i = 0; i < 4; i++)
                    logged_in |= users[i] > 0;
            int idle = !uc_bt_running() && !uc_usb_running() && !bt_start_queued &&
                       !usb_start_queued && !uc_core_active_session() && !enable_pending;
            uc_launcher_poll(millis(), uc_backend_power_state() == 0 && logged_in, idle);
        }
        if (settings.bt_auto) {
            int idle = !uc_bt_running() && !bt_start_queued && !uc_usb_running() &&
                       !usb_start_queued && !uc_core_active_session() && !enable_pending;
            if (uc_backend_auto_due(settings.bt_auto_user, idle)) {
                int rc = settings.bt_auto_resume ? uc_bt_start_system_assigned()
                                                 : uc_bt_start(settings.bt_auto_user);
                uc_log_info("[bluetooth-auto] start result=%d user=%d; no automatic retry", rc,
                            settings.bt_auto_user);
            }
        }
        if (bt_start_queued && millis() >= bt_start_at) {
            bt_start_queued = 0;
            int rc = uc_bt_start(bt_start_user);
            if (rc == 0 && settings.bt_auto_user != bt_start_user) {
                Settings next = settings;
                next.bt_auto_user = bt_start_user;
                if (save_settings(&next) == 0) {
                    settings = next;
                    uc_log_info("[bluetooth-auto] remembered user=%d", bt_start_user);
                } else {
                    uc_log_warn(
                        "[bluetooth-auto] could not persist user; previous account retained");
                }
            }
        }
        if (usb_start_queued && millis() >= usb_start_at) {
            usb_start_queued = 0;
            int rc = uc_usb_start(usb_start_user);
            uc_log_info("[usb] queued start result=%d user=%d", rc, usb_start_user);
        }
        uc_bt_poll(millis());
        uc_usb_poll(millis());
        uc_core_tick(millis());
        if (active_session && !uc_core_session_active(active_session))
            active_session = 0;
        if (!active_session)
            active_user = -1;
        for (int count = 0; count < CLIENTS; count++) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0)
                break;
            int slot = -1;
            for (int i = 0; i < CLIENTS; i++)
                if (clients[i].fd < 0) {
                    slot = i;
                    break;
                }
            if (slot < 0 || nonblock(fd)) {
                close(fd);
                continue;
            }
            Client *c = &clients[slot];
            c->fd = fd;
            c->stop_after_send = 0;
            c->used = c->sent = c->total = 0;
            c->deadline = millis() + 3000;
        }
        uint64_t now = millis();
        for (int i = 0; i < CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd < 0)
                continue;
            if (now >= c->deadline) {
                close_client(c);
                continue;
            }
            if (c->total) {
                ssize_t n = send(c->fd, c->out + c->sent, c->total - c->sent, 0);
                if (n > 0) {
                    c->sent += (size_t)n;
                    if (c->sent == c->total) {
                        if (c->stop_after_send)
                            stop_requested = 1;
                        close_client(c);
                    }
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    close_client(c);
            } else {
                ssize_t n = recv(c->fd, c->in + c->used, REQUEST_MAX - c->used, 0);
                if (n > 0) {
                    c->used += (size_t)n;
                    parse_request(c);
                    if (c->fd >= 0 && !c->total && c->used == REQUEST_MAX)
                        error_response(c, 413, "request_too_large");
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    close_client(c);
            }
        }
        struct timespec delay = {0, 5000000};
        nanosleep(&delay, NULL);
    }
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].fd >= 0)
            close_client(&clients[i]);
    close(listener);
    bt_start_queued = 0;
    usb_start_queued = 0;
    (void)uc_bt_stop();
    (void)uc_usb_stop();
    uc_core_disable();
    uc_backend_cancel_pending();
    return 0;
}
