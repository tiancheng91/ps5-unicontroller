#ifndef UC_DEVICE_OPTIONS_H
#define UC_DEVICE_OPTIONS_H
#include <stddef.h>
#include <stdint.h>
#define UC_DEVICE_KEY_MAX 96
#define UC_DEVICE_OPTIONS_MAX 64
typedef struct {
    int rumble, swap_ab;
} uc_device_options;
/* Single event-loop owner. Missing entries use known-device rumble and no flip.
 * File writes are atomic; failed writes never replace live preferences. */
int uc_device_options_load(const char *path);
uc_device_options uc_device_options_get(const char *key, int known);
int uc_device_options_save(const char *key, uc_device_options options);
uint32_t uc_device_options_buttons(uint32_t buttons, int swap);
#endif
