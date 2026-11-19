#include "../../src/drivers/config/overrides.h"
#include "../../src/drivers/protocols/profiles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char valid[] = "{\"schema_version\":1,\"devices\":[{\"match\":{\"transport\":"
                            "\"classic\",\"vid\":\"1234\",\"pid\":\"5678\"},\"profile\":\"generic-"
                            "hid\",\"name\":\"Test pad\",\"mapping\":{\"button1\":\"touchpad\"}}]}";
int main(void)
{
    assert(profile_device_known(PAD_TRANSPORT_CLASSIC, 0x057e, 0x2009));
    assert(profile_device_known(PAD_TRANSPORT_USB, 0x3537, 0x1041));
    assert(!profile_device_known(PAD_TRANSPORT_USB, 0x1234, 0x5678));
    /* An imported SDL name must not enable protocol quirks, BLE or default rumble. */
    assert(profile_model_name(0x054c, 0x0268) == NULL);
    assert(!strcmp(profile_model_name(0x2dc8, 0x6000), "8Bitdo SF30 Controller"));
    assert(!profile_device_known(PAD_TRANSPORT_CLASSIC, 0x2dc8, 0x6000));
    /* No local 8BitDo layout overrides SDL family selection. */
    assert(!profile_hid_mapping(0x2dc8, 0x3106));
    assert(!profile_hid_mapping(0x2dc8, 0x6001));
    assert(profile_classic_default(0x2dc8, 0x3106, NULL, NULL) == profile_builtin_named("xbox"));
    assert(profile_classic_unsupported(0x2dc8, 0x6001));
    assert(!profile_device_known(PAD_TRANSPORT_CLASSIC, 0x2dc8, 0x6001));
    assert(profile_classic_unsupported(0x054c, 0x0268));
    assert(!profile_device_known(PAD_TRANSPORT_USB, 0x054c, 0x0268));
    assert(!profile_device_known(PAD_TRANSPORT_CLASSIC, 0x054c, 0x0268));
    assert(!profile_find(0x054c, 0x0268));
    assert(!profile_ble_allowed(0x054c, 0x0268));
    assert(profile_ble_allowed(0x045e, 0x0b13));
    assert(!profile_ble_allowed(0x057e, 0x2069));
    assert(!profile_ble_allowed(0x045e, 0x02e0));
    assert(!profile_ble_allowed(0x057e, 0x2009));
    assert(!profile_ble_allowed(0x054c, 0x09cc));
    assert(!profile_ble_allowed(0x1234, 0x5678));
    uc_controller_overrides *out = calloc(1, sizeof *out);
    char error[160];
    assert(out);
    assert(!uc_controller_json_parse((const unsigned char *)valid, strlen(valid), out, error,
                                     sizeof error));
    assert(out->count == 1 && !strcmp(out->devices[0].mapping, "button1=touchpad\n"));
    const char *bad[] = {"{}", "{\"schema_version\":1,\"schema_version\":1,\"devices\":[]}",
                         "{\"schema_version\":1,\"devices\":[],\"invert\":true}", "[]", "null"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        assert(uc_controller_json_parse((const unsigned char *)bad[i], strlen(bad[i]), out, error,
                                        sizeof error) == -1);
        assert(!out->count);
    }
    char path[] = "/tmp/uc-overrides-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, valid, strlen(valid)) == (ssize_t)strlen(valid));
    close(fd);
    assert(uc_controller_overrides_load(path, error, sizeof error) == 1);
    assert(uc_controller_override_find(PAD_TRANSPORT_CLASSIC, 0x1234, 0x5678));
    assert(!uc_controller_override_find(PAD_TRANSPORT_BLE, 0x1234, 0x5678));
    assert(
        !strcmp(pad_display_name_for_transport(PAD_TRANSPORT_CLASSIC, 0x1234, 0x5678, "fallback"),
                "Test pad"));
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("{}", f);
    fclose(f);
    assert(uc_controller_overrides_load(path, error, sizeof error) == -1);
    assert(!uc_controller_override_find(PAD_TRANSPORT_CLASSIC, 0x1234, 0x5678));
    const char *identity = "{\"schema_version\":1,\"devices\":["
                           "{\"match\":{\"transport\":\"classic\",\"vid\":\"0000\",\"pid\":"
                           "\"0000\"},\"profile\":\"generic-hid\"},"
                           "{\"match\":{\"transport\":\"classic\",\"name\":\"Lic Pro "
                           "Controller\"},\"profile\":\"switch\"},"
                           "{\"match\":{\"transport\":\"classic\",\"address\":\"30:31:7D:6E:97:"
                           "F6\"},\"profile\":\"xbox\"}]}";
    f = fopen(path, "w");
    assert(f);
    fputs(identity, f);
    fclose(f);
    assert(uc_controller_overrides_load(path, error, sizeof error) == 1);
    const uint8_t mac[6] = {0xf6, 0x97, 0x6e, 0x7d, 0x31, 0x30};
    int probe = 0;
    const pad_profile *p = profile_classic_candidate(0, 0, mac, "Lic Pro Controller", &probe);
    assert(p == profile_builtin_named("xbox") && !probe);
    p = profile_classic_candidate(0, 0, NULL, "Lic Pro Controller", &probe);
    assert(p == profile_builtin_named("switch") && probe);
    assert(!profile_classic_candidate(0, 0, NULL, "Other", &probe));
    assert(!uc_controller_override_find(PAD_TRANSPORT_BLE, 0, 0));
    assert(uc_controller_override_needs_name(0x045e, 0x02fd, mac));
    assert(profile_classic_candidate(0x054c, 0x0ce6, mac, "Lic Pro Controller", &probe) == NULL);
    const char *bad_match[] = {"\"transport\":\"classic\"",
                               "\"transport\":\"classic\",\"vid\":\"0000\"",
                               "\"transport\":\"classic\",\"address\":\"30:31:7D:6E:97:FG\"",
                               "\"transport\":\"ble\",\"name\":\"Lic Pro Controller\"",
                               "\"transport\":\"usb\",\"address\":\"30:31:7D:6E:97:F6\""};
    for (size_t i = 0; i < sizeof bad_match / sizeof bad_match[0]; i++) {
        char json[512];
        snprintf(json, sizeof json,
                 "{\"schema_version\":1,\"devices\":[{\"match\":{%s},\"profile\":\"switch\"}]}",
                 bad_match[i]);
        assert(uc_controller_json_parse((const unsigned char *)json, strlen(json), out, error,
                                        sizeof error) == -1);
    }
    uc_controller_overrides_clear();
    /* SDL types select only implemented Classic candidates, not USB/other families. */
    p = profile_classic_candidate(0x20d6, 0xa711, NULL, NULL, &probe);
    assert(p == profile_builtin_named("switch") && probe);
    assert(!profile_classic_unsupported(0x20d6, 0xa711));
    assert(!profile_ble_allowed(0x20d6, 0xa711));
    p = profile_classic_candidate(0x0e6f, 0x0186, NULL, NULL, &probe);
    assert(p == profile_builtin_named("switch") && probe);
    p = profile_classic_candidate(0x045e, 0x02e3, NULL, NULL, &probe);
    assert(p == profile_builtin_named("xbox") && !probe);
    p = profile_classic_candidate(0x1532, 0x1000, NULL, NULL, &probe);
    assert(p == profile_builtin_named("ds4") && !probe);
    p = profile_classic_candidate(0x0f0d, 0x0184, NULL, NULL, &probe);
    assert(p == profile_builtin_named("ds5") && !probe);
    assert(!profile_classic_candidate(0x045e, 0x028e, NULL, NULL, &probe));
    assert(profile_classic_unsupported(0x045e, 0x028e));
    assert(profile_classic_unsupported(0x0e6f, 0x0193));
    assert(!profile_ble_allowed(0x0e6f, 0x0193));
    assert(!profile_classic_unsupported(0x1234, 0x5678));
    p = profile_classic_candidate(0, 0, NULL, "Lic Pro Controller", &probe);
    assert(p == profile_builtin_named("switch") && probe);
    const char *names[] = {"Lic Pro Controller", "Nintendo Wireless Gamepad", "Wireless Gamepad"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        p = profile_classic_candidate(0, 0, NULL, names[i], &probe);
        assert(p == profile_builtin_named("switch") && probe);
        assert(!profile_classic_candidate(1, 1, NULL, names[i], &probe) && !probe);
        p = profile_classic_candidate(0x045e, 0x02fd, NULL, names[i], &probe);
        assert(p == profile_builtin_named("xbox") && !probe);
    }
    assert(!profile_classic_candidate(0, 0, NULL, NULL, &probe) && !probe);
    assert(!profile_classic_candidate(0, 0, NULL, "wireless gamepad", &probe) && !probe);
    assert(!profile_classic_candidate(0, 0, NULL, "Lic Pro Controller X", &probe));
    assert(!profile_classic_candidate(0x1234, 0x5678, NULL, "Lic Pro Controller", &probe));
    /* A mapping-only rule cannot silently select an SDL-only dedicated codec. */
    for (unsigned explicit_generic = 0; explicit_generic < 2; explicit_generic++) {
        f = fopen(path, "w");
        assert(f);
        fprintf(f,
                "{\"schema_version\":1,\"devices\":[{\"match\":{\"transport\":"
                "\"classic\",\"vid\":\"1532\",\"pid\":\"1000\"},%s"
                "\"mapping\":{\"button1\":\"touchpad\"}}]}",
                explicit_generic ? "\"profile\":\"generic-hid\"," : "");
        fclose(f);
        assert(uc_controller_overrides_load(path, error, sizeof error) ==
               (explicit_generic ? 1 : -1));
        if (explicit_generic)
            assert(!profile_classic_candidate(0x1532, 0x1000, NULL, NULL, &probe));
        else
            assert(strstr(error, "profile=generic-hid"));
    }
    uc_controller_overrides_clear();
    /* Maintained HID layouts remain descriptor-based, regardless of SDL names. */
    assert(profile_hid_mapping(0x20d6, 0x89e5));
    assert(!profile_classic_default(0x20d6, 0x89e5, NULL, &probe));
    f = fopen(path, "w");
    assert(f);
    fputs("{\"schema_version\":1,\"devices\":[{\"match\":{\"transport\":\"ble\","
          "\"vid\":\"057e\",\"pid\":\"2069\"},\"ble\":true}]}",
          f);
    fclose(f);
    assert(uc_controller_overrides_load(path, error, sizeof error) == 1);
    assert(!profile_ble_allowed(0x057e, 0x2069));
    uc_controller_overrides_clear();
    unlink(path);
    assert(!uc_controller_overrides_load(path, error, sizeof error));
    free(out);
    puts("JSON overrides: parsing, scoped lookup and atomic fallback passed");
    return 0;
}
