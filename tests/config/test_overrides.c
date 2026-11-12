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
    assert(profile_ble_allowed(0x045e, 0x0b13));
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
    unlink(path);
    assert(!uc_controller_overrides_load(path, error, sizeof error));
    free(out);
    puts("JSON overrides: parsing, scoped lookup and atomic fallback passed");
    return 0;
}
