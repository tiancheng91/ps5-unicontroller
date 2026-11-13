#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "../../src/core/device_options.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void)
{
    char directory[] = "/tmp/uc-device-options-XXXXXX";
    assert(mkdtemp(directory));
    char path[1024];
    snprintf(path, sizeof path, "%s/device-settings.json", directory);
    assert(!uc_device_options_load(path));
    const char *first = "usb:/dev/ugen1.2:3537:1040";
    const char *second = "usb:/dev/ugen1.3:3537:1040";
    assert(uc_device_options_get(first, 1).rumble);
    assert(!uc_device_options_get(first, 0).rumble);
    assert(!uc_device_options_get(first, 1).swap_ab);
    assert(!uc_device_options_save(first, (uc_device_options){0, 1}));
    assert(!uc_device_options_get(first, 1).rumble && uc_device_options_get(first, 1).swap_ab);
    assert(uc_device_options_get(second, 1).rumble && !uc_device_options_get(second, 1).swap_ab);
    assert(!uc_device_options_save(second, (uc_device_options){1, 0}));
    assert(!uc_device_options_load(path));
    assert(uc_device_options_get(first, 0).swap_ab && uc_device_options_get(second, 0).rumble);
    assert(!uc_device_options_get("usb:/dev/ugen1.2:3537:1041", 0).swap_ab);
    assert(!uc_device_options_save("ble:01:02:03:04:05:06:045E:0B13", (uc_device_options){1, 1}));
    assert(!uc_device_options_get("classic:01:02:03:04:05:06:045E:0B13", 0).rumble);
    assert(uc_device_options_save("bad\"key", (uc_device_options){0, 0}) < 0);
    assert(uc_device_options_save(first, (uc_device_options){2, 0}) < 0);
    assert(uc_device_options_get(first, 1).swap_ab);
    uint32_t original = (1u << 14) | (1u << 15) | (1u << 16) | 1;
    uint32_t swapped = (1u << 13) | (1u << 12) | (1u << 16) | 1;
    assert(uc_device_options_buttons(original, 0) == original);
    assert(uc_device_options_buttons(original, 1) == swapped);
    assert(uc_device_options_buttons(swapped, 1) == original);
    /* Failed persistence cannot leak a changed option into the running process. */
    assert(!unlink(path) && !rmdir(directory));
    assert(uc_device_options_save(first, (uc_device_options){1, 0}) < 0);
    assert(uc_device_options_get(first, 1).swap_ab);
    assert(mkdir(directory, 0700) == 0);
    FILE *file = fopen(path, "w");
    assert(file);
    fputs("{\"schema_version\":1,\"devices\":[{\"key\":\"usb:a\",\"rumble\":true,\"rumble\":false,"
          "\"swap_ab\":false}]}",
          file);
    fclose(file);
    assert(uc_device_options_load(path) < 0);
    /* No partially loaded document survives rejection. */
    assert(!uc_device_options_get(first, 0).rumble);
    assert(!unlink(path));
    assert(!uc_device_options_load(path));
    for (unsigned i = 0; i < UC_DEVICE_OPTIONS_MAX; ++i) {
        char key[64];
        snprintf(key, sizeof key, "usb:port%u:3537:1040", i);
        assert(!uc_device_options_save(key, (uc_device_options){0, 1}));
    }
    assert(uc_device_options_save("usb:overflow", (uc_device_options){0, 0}) < 0);
    assert(!uc_device_options_load(path));
    assert(uc_device_options_get("usb:port63:3537:1040", 0).swap_ab);
    assert(!unlink(path) && !rmdir(directory));
    puts("Device options: defaults, identity isolation, persistence, atomic failure, bounds and "
         "layout PASS");
    return 0;
}
