/* End-to-end runs of the LE half of the host against the simulated LE
 * controller and pad. */
#include "sim_le.h"
#include "../../src/drivers/config/overrides.h"
#include "../../src/drivers/bluetooth/host.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                  \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    int connects, disconnects, states, slot;
    pad_info info;
    pad_state last;
} seen;

static void on_connect(void *ud, int slot, const pad_info *info)
{
    seen *s = ud;
    s->connects++;
    s->slot = slot;
    s->info = *info;
}

static void on_state(void *ud, int slot, const pad_state *st)
{
    seen *s = ud;
    (void)slot;
    s->states++;
    s->last = *st;
}

static void on_disconnect(void *ud, int slot)
{
    seen *s = ud;
    (void)slot;
    s->disconnects++;
}

static const char *DB = "build/test_le.db";

static void run(host_t *h, int ms)
{
    long end = sim_le_now() + ms;
    while (sim_le_now() < end)
        host_poll(h, 5);
}

/* Existing protocol tests select the first recorded result explicitly. New
 * discovery-only tests below separately prove scans never connect. */
static int select_first(host_t *h, int seconds)
{
    if (!host_scan(h, seconds))
        return 0;
    long until = sim_le_now() + 5000;
    while (!host_discovered_count(h) && sim_le_now() < until)
        host_poll(h, 5);
    host_discovered_info found;
    return host_discovered_get(h, 0, &found) && host_pair_device(h, found.id);
}

static host_t *open_host(sim_le_t *sim, seen *s)
{
    host_events ev = {on_connect, on_state, on_disconnect, s, NULL};
    return host_open(sim_le_hci(sim), DB, &ev);
}

/* An Xbox Series controller: report 1 is 16 bytes after its id. A is
 * pressed, Share too, the left stick is pushed right. */
static void xbox_pad(sim_le_pad *p, int sc, int rpa)
{
    static const uint8_t addr[6] = {0x51, 0x52, 0x53, 0x54, 0x55, 0xD6};
    static const uint8_t report[16] = {
        0xFF, 0xFF, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, /* sticks */
        0x00, 0x00, 0x00, 0x00,                         /* triggers */
        0x00,                                           /* hat */
        0x01, 0x00, 0x01,                               /* A; Share */
    };
    int i;

    memset(p, 0, sizeof *p);
    memcpy(p->addr, addr, 6);
    p->use_rpa = rpa;
    for (i = 0; i < 16; i++)
        p->irk[i] = (uint8_t)(0x90 + i);
    p->sc = sc;
    p->vid = 0x045E;
    p->pid = 0x0B13;
    p->map[0] = 0x05; /* never read: the profile knows the pad */
    p->map_len = 1;
    memcpy(p->report, report, sizeof report);
    p->report_len = sizeof report;
    p->rid_in = 1;
    p->rid_out = 3;
    p->mtu = 185;
}

static void test_scan_response_and_inquiry(void)
{
    unlink(DB);
    sim_le_pad pad;
    xbox_pad(&pad, 1, 0);
    sim_le_t *sim = sim_le_new(&pad);
    seen observed = {0};
    host_t *h = open_host(sim, &observed);
    CHECK(h && host_scan(h, 10));
    if (!h) {
        sim_le_free(sim);
        return;
    }
    run(h, 50);
    host_discovered_info found;
    CHECK(host_discovered_get(h, 0, &found));
    /* A harmless non-connectable report precedes the name-only scan response. */
    unsigned char reports[64] = {2, 2, 3, 0, 1, 2, 3, 4, 5, 6, 0, 0xc0};
    int offset = 12;
    const char *name = "Xbox Wireless Controller";
    int length = (int)strlen(name);
    reports[offset] = 4;
    reports[offset + 1] = 0;
    memcpy(reports + offset + 2, pad.addr, 6);
    reports[offset + 8] = (unsigned char)(length + 2);
    reports[offset + 9] = (unsigned char)(length + 1);
    reports[offset + 10] = 9;
    memcpy(reports + offset + 11, name, (size_t)length);
    reports[offset + 11 + length] = 0xc0;
    sim_le_inject_event(sim, 0x3e, reports, offset + 12 + length);
    run(h, 5);
    CHECK(host_discovered_count(h) == 1);
    CHECK(host_discovered_get(h, 0, &found) && !strcmp(found.name, name));
    CHECK(sim_le_creates(sim) == 0);
    CHECK(host_pair_device(h, found.id));
    run(h, 300);
    CHECK(sim_le_creates(sim) == 0); /* Wait for Classic inquiry; no conflicting create. */
    run(h, 8000);
    CHECK(sim_le_creates(sim) == 1 && observed.connects == 1);
    CHECK(sim_le_cancels(sim) == 0);
    host_close(h);
    sim_le_free(sim);
    unlink(DB);
}

static void test_manual_le_discovery(void)
{
    unlink(DB);
    sim_le_pad pad;
    xbox_pad(&pad, 1, 0);
    sim_le_t *sim = sim_le_new(&pad);
    seen observed = {0};
    host_t *h = open_host(sim, &observed);
    CHECK(h && host_scan(h, 10));
    if (!h) {
        sim_le_free(sim);
        return;
    }
    run(h, 2000);
    CHECK(host_discovered_count(h) == 1);
    CHECK(sim_le_creates(sim) == 0 && sim_le_pairings(sim) == 0 && observed.connects == 0);
    host_discovered_info found;
    CHECK(host_discovered_get(h, 0, &found) && found.le);
    CHECK(host_scan(h, 0));
    run(h, 1000);
    CHECK(sim_le_creates(sim) == 0);
    CHECK(host_pair_device(h, found.id));
    run(h, 8000);
    CHECK(observed.connects == 1 && sim_le_paired(sim));
    host_close(h);
    sim_le_free(sim);
    unlink(DB);
}

static void test_xbox_sc(void)
{
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;
    uint8_t out[16];

    printf("Xbox Series over LE: Secure Connections pairing, GATT, reports, rumble\n");
    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 1, 0);
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h)
        return;
    select_first(h, 30);
    run(h, 8000);

    CHECK(sim_le_error(sim) == NULL);
    if (sim_le_error(sim))
        printf("  pad says: %s\n", sim_le_error(sim));
    CHECK(sim_le_paired(sim));
    CHECK(sim_le_encrypted(sim));
    CHECK(s.connects == 1);
    CHECK(s.info.vid == 0x045E && s.info.pid == 0x0B13);
    CHECK(s.info.profile && strcmp(s.info.profile, "Xbox") == 0);
    CHECK(s.states > 0);
    CHECK((s.last.buttons & 0x4000) && (s.last.buttons & 0x1)); /* cross, create */
    CHECK(s.last.lx == 0xFF && s.last.ly == 0x80);
    CHECK(host_paired_count(h) == 1);

    {
        pad_output o = {255, 51, 0, 0, 0, 1, 0};
        CHECK(host_set_output(h, s.slot, &o));
        CHECK(sim_le_outputs(sim) == 1);
        CHECK(sim_le_last_output(sim, out, sizeof out) == 8);
        CHECK(out[0] == 0x0F && out[3] == 100 && out[4] == 20 && out[5] == 0xFF);
    }

    { /* the web page's rumble test: a 500 ms pulse that ends by itself
       * (not repeated), then a report that stops it */
        int before = sim_le_outputs(sim);
        CHECK(host_rumble_pulse(h, s.slot, 500));
        CHECK(sim_le_outputs(sim) == before + 1);
        CHECK(sim_le_last_output(sim, out, sizeof out) == 8);
        CHECK(out[0] == 0x0F && out[3] == 78 && out[4] == 78 && out[5] == 55 && out[7] == 0);
        run(h, 300);
        CHECK(sim_le_outputs(sim) == before + 1);
        run(h, 300);
        CHECK(sim_le_outputs(sim) == before + 2);
        CHECK(sim_le_last_output(sim, out, sizeof out) == 8);
        CHECK(out[3] == 0 && out[4] == 0 && out[5] == 0xFF && out[7] == 0xEB);
        run(h, 1000);
        CHECK(sim_le_outputs(sim) == before + 2);
        CHECK(!host_rumble_pulse(h, s.slot + 1, 500));
    }

    {
        int before = sim_le_outputs(sim);

        printf("game rumble: Xbox device-side timeout, renewal and stale-feedback stop\n");
        CHECK(host_rumble_feedback(h, s.slot, 255, 51));
        CHECK(sim_le_outputs(sim) == before + 1);
        CHECK(sim_le_last_output(sim, out, sizeof out) == 8);
        CHECK(out[3] == 100 && out[4] == 20 && out[5] == 60 && out[7] == 0);
        run(h, 200);
        CHECK(sim_le_outputs(sim) == before + 1);
        CHECK(host_rumble_feedback(h, s.slot, 255, 51));
        CHECK(sim_le_outputs(sim) == before + 1); /* No duplicate immediate send. */
        run(h, 100);
        CHECK(sim_le_outputs(sim) == before + 2);
        run(h, 800);
        CHECK(sim_le_last_output(sim, out, sizeof out) == 8);
        CHECK(out[3] == 0 && out[4] == 0);
        before = sim_le_outputs(sim);
        run(h, 1000);
        CHECK(sim_le_outputs(sim) == before);
    }
    sim_le_power_off(sim);
    run(h, 200);
    CHECK(s.disconnects == 1);
    host_close(h);
    sim_le_free(sim);
}

static void test_reconnect_rpa(void)
{
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;

    printf("LE pad behind private addresses: paired, off, on again, back with its keys\n");
    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 1, 1);
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    select_first(h, 10);
    run(h, 8000);
    CHECK(s.connects == 1 && sim_le_pairings(sim) == 1);

    sim_le_power_off(sim);
    run(h, 3000);         /* the pairing window closes */
    sim_le_power_on(sim); /* a new private address */
    run(h, 8000);
    CHECK(sim_le_error(sim) == NULL);
    if (sim_le_error(sim))
        printf("  pad says: %s\n", sim_le_error(sim));
    CHECK(s.connects == 2);
    CHECK(sim_le_pairings(sim) == 1); /* no second pairing: the stored LTK */
    CHECK(sim_le_encrypted(sim));
    CHECK(s.last.buttons & 0x4000);

    /* And after the host restarts, from the store. */
    host_close(h);
    sim_le_power_off(sim);
    memset(&s, 0, sizeof s);
    h = open_host(sim, &s);
    sim_le_power_on(sim);
    run(h, 8000);
    CHECK(s.connects == 1 && sim_le_pairings(sim) == 1);
    host_close(h);
    sim_le_free(sim);
}

/* A pad no profile knows, with legacy pairing only and a 23-byte ATT MTU,
 * so its report map comes in several blobs. */
static const uint8_t k_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95,
    0x04, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02, 0x15, 0x00, 0x25, 0x07, 0x75,
    0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x02, 0x15,
    0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x09, 0xC5, 0x09, 0xC4, 0x81, 0x02, 0xC0,
};

static void test_generic_legacy(void)
{
    static const uint8_t report[9] = {0x00, 0x80, 0x80, 0x80, 0x06, 0x02, 0x00, 0xFF, 0x00};
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;

    const char *config = "build/ble-test.json";
    FILE *f = fopen(config, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs("{\"schema_version\":1,\"devices\":[{\"match\":{\"transport\":\"ble\",\"vid\":\"2dc8\","
          "\"pid\":\"9021\"},\"ble\":true}]}",
          f);
    fclose(f);
    char error[160];
    CHECK(uc_controller_overrides_load(config, error, sizeof error) == 1);
    unlink(config);
    printf("unknown LE pad: legacy pairing, report map in blobs, generic profile\n");
    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 0, 0);
    pad.vid = 0x2DC8;
    pad.pid = 0x9021;
    memcpy(pad.map, k_desc, sizeof k_desc);
    pad.map_len = sizeof k_desc;
    memcpy(pad.report, report, sizeof report);
    pad.report_len = sizeof report;
    pad.mtu = 23;
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    select_first(h, 30);
    run(h, 10000);
    CHECK(sim_le_error(sim) == NULL);
    if (sim_le_error(sim))
        printf("  pad says: %s\n", sim_le_error(sim));
    CHECK(sim_le_paired(sim));
    CHECK(sim_le_blob_reads(sim) >= 3);
    CHECK(s.connects == 1);
    CHECK(s.info.profile && strcmp(s.info.profile, "Generic HID") == 0);
    CHECK(s.last.lx == 0 && (s.last.buttons & 0x80));    /* hat west: left */
    CHECK(s.last.buttons & 0x2000);                      /* button 2: circle */
    CHECK(s.last.l2 == 255 && (s.last.buttons & 0x100)); /* brake */
    host_hid_descriptor descriptor;
    CHECK(host_hid_descriptor_get(h, s.slot, &descriptor));
    CHECK(descriptor.length == pad.map_len && !descriptor.truncated);
    CHECK(!memcmp(descriptor.data, pad.map, (size_t)pad.map_len));
    host_close(h);

    /* Legacy bonds reconnect with the distributed LTK, EDIV and Rand. */
    sim_le_power_off(sim);
    memset(&s, 0, sizeof s);
    h = open_host(sim, &s);
    sim_le_power_on(sim);
    run(h, 8000);
    CHECK(s.connects == 1 && sim_le_pairings(sim) == 1);
    CHECK(sim_le_error(sim) == NULL);
    host_close(h);
    sim_le_free(sim);
    uc_controller_overrides_clear();
}

static void test_garbage(void)
{
    static const unsigned cids[] = {0x0004, 0x0005, 0x0006, 0x0040};
    static const uint8_t codes[] = {0x05, 0x08, 0x0E, 0x0F, 0x13, 0x30, 0x3E};
    unsigned seed = 4242;
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;
    int i;

    printf("garbage ATT, SMP, signaling and LE events on a working LE link\n");
    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 1, 0);
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    select_first(h, 30);
    run(h, 8000);
    CHECK(s.connects == 1);
    for (i = 0; i < 20000; i++) {
        uint8_t junk[300];
        int n, k;
        seed = seed * 1103515245u + 12345u;
        n = (int)((seed >> 8) % 280);
        for (k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            junk[k] = (uint8_t)(seed >> 16);
        }
        if (i % 4 == 3)
            sim_le_inject_event(sim, codes[(seed >> 4) % sizeof codes], junk, n < 255 ? n : 255);
        else
            sim_le_inject_acl(sim, cids[(seed >> 4) % 4], junk, n);
        if (i % 16 == 0)
            host_poll(h, 1);
    }
    run(h, 500);
    printf("  connects %d, disconnects %d, connected now %d\n", s.connects, s.disconnects,
           host_connected(h, s.slot));
    /* A garbage Disconnection Complete for our handle ends the link, and the
     * paired pad then reconnects: the count must simply add up. */
    CHECK(host_connected(h, s.slot) == (s.connects - s.disconnects == 1));
    host_close(h);
    sim_le_free(sim);
}

/* A foreign pending initiator may belong to the console. Refusal must not
 * authorize cancellation; our bounded attempt ends visibly instead. */
static void test_refused_then_ok(void)
{
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;

    printf("LE busy (0x0c): do not cancel system-owned pending connection\n");
    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 1, 0);
    pad.foreign_pending = 1;
    pad.foreign_restarts = 1; /* started again after the first cancel */
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h)
        return;
    select_first(h, 30);
    run(h, 8000);
    CHECK(sim_le_error(sim) == NULL);
    CHECK(s.connects == 0);
    CHECK(sim_le_creates(sim) == 4);
    CHECK(sim_le_cancels(sim) == 0);
    printf("  creates %d, cancels %d, connects %d\n", sim_le_creates(sim), sim_le_cancels(sim),
           s.connects);
    host_close(h);
    sim_le_free(sim);
}

/* Refused on every try: a few tries, then the address rests; no loop. */
static void test_refused_backoff(void)
{
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;

    printf("LE Create Connection refused on every try: backoff, then a rest\n");
    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 1, 0);
    pad.foreign_pending = 1;
    pad.foreign_restarts = 7; /* clears on the 8th cancel */
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h)
        return;
    select_first(h, 60);
    run(h, 4000);
    CHECK(sim_le_creates(sim) == 4); /* the first try and three more */
    CHECK(sim_le_cancels(sim) == 0);
    CHECK(s.connects == 0);
    run(h, 3000);
    CHECK(sim_le_creates(sim) == 4); /* resting */
    run(h, 20000);
    CHECK(sim_le_creates(sim) == 4); /* No advertisement-triggered second pairing. */
    CHECK(sim_le_cancels(sim) == 0);
    CHECK(s.connects == 0);
    sim_le_finish_foreign(sim); /* System completes its own operation. */
    host_discovered_info found;
    CHECK(host_discovered_get(h, 0, &found));
    CHECK(host_pair_device(h, found.id)); /* Explicit retry, not auto-pair. */
    run(h, 8000);
    CHECK(s.connects == 1 && sim_le_paired(sim));
    CHECK(sim_le_cancels(sim) == 0);
    printf("  creates %d, cancels %d, connects %d\n", sim_le_creates(sim), sim_le_cancels(sim),
           s.connects);
    host_close(h);
    sim_le_free(sim);
}

static int pair_once(int drop_every, unsigned seed)
{
    sim_le_pad pad;
    sim_le_t *sim;
    host_t *h;
    seen s;
    int ok;

    unlink(DB);
    memset(&s, 0, sizeof s);
    xbox_pad(&pad, 1, 0);
    pad.drop_every = drop_every;
    pad.seed = seed;
    sim = sim_le_new(&pad);
    h = open_host(sim, &s);
    if (!h) {
        sim_le_free(sim);
        return 0;
    }
    select_first(h, 60);
    for (int click = 0; click < 5 && !s.connects; click++) {
        /* User retries do not follow a radio-period-aligned 15s cadence. */
        run(h, 8000 + (int)((seed >> click) % 3000));
        host_discovered_info found;
        if (click < 4 && !s.connects && host_discovered_get(h, 0, &found) &&
            !strcmp(found.state, "failed"))
            (void)host_pair_device(h, found.id); /* Simulated user retry, never advertisement. */
    }
    ok = s.connects >= 1 && s.states > 0 && (s.last.buttons & 0x4000);
    if (!ok) {
        host_discovered_info found;
        CHECK(host_discovered_get(h, 0, &found));
        CHECK(!strcmp(found.state, "failed") && found.reason[0]);
        int creates = sim_le_creates(sim);
        run(h, 7000);
        CHECK(sim_le_creates(sim) == creates); /* No hidden automatic re-pairing. */
        printf("  seed %u: failed visibly after explicit user retries (%s); no auto-pair loop\n",
               seed, found.reason);
        ok = !strcmp(found.state, "failed") && found.reason[0] && sim_le_creates(sim) == creates
                 ? 2
                 : 0;
    }
    host_close(h);
    sim_le_free(sim);
    return ok;
}

static void test_stress(void)
{
    int ok = 0, reported_failures = 0, runs = 30;
    unsigned seed;

    printf("LE manual pairing under 1-in-10 packet loss, %d runs (up to five explicit clicks)\n",
           runs);
    for (seed = 1; seed <= (unsigned)runs; seed++) {
        int outcome = pair_once(10, seed * 104729);
        ok += outcome == 1;
        reported_failures += outcome == 2;
    }
    printf("  %d/%d working; %d visible failures requiring user action, no automatic re-pairing\n",
           ok, runs, reported_failures);
    CHECK(ok + reported_failures == runs && ok > 0);
}

static void test_ble_rejected_without_saved_pairing(uint16_t vid, uint16_t pid)
{
    sim_le_pad pad;
    seen observed = {0};
    unlink(DB);
    xbox_pad(&pad, 1, 0);
    pad.vid = vid;
    pad.pid = pid;
    sim_le_t *sim = sim_le_new(&pad);
    host_t *host = open_host(sim, &observed);
    CHECK(host != NULL);
    if (!host) {
        sim_le_free(sim);
        return;
    }
    select_first(host, 10);
    run(host, 8000);
    CHECK(observed.connects == 0 && observed.states == 0);
    CHECK(host_paired_count(host) == 0);
    host_close(host);
    sim_le_free(sim);
    unlink(DB);
}

int main(void)
{
    test_scan_response_and_inquiry();
    test_manual_le_discovery();
    test_ble_rejected_without_saved_pairing(0x057e, 0x2009);
    test_ble_rejected_without_saved_pairing(0x1234, 0x5678);
    test_ble_rejected_without_saved_pairing(0x054c, 0x09cc);
    test_xbox_sc();
    test_reconnect_rpa();
    test_generic_legacy();
    test_garbage();
    test_refused_then_ok();
    test_refused_backoff();
    test_stress();
    printf(g_fail ? "%d check(s) failed\n" : "all LE checks passed\n", g_fail);
    return g_fail != 0;
}
