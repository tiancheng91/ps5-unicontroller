#ifndef UC_CONTROLLER_OVERRIDES_H
#define UC_CONTROLLER_OVERRIDES_H
#include <stddef.h>
#include <stdint.h>
#include "../../core/pad.h"
#define UC_OVERRIDE_MAX 32
#define UC_OVERRIDE_FILE_MAX (32u * 1024u)
typedef pad_transport uc_controller_transport;
#define UC_CONTROLLER_BUILTIN PAD_TRANSPORT_UNSPECIFIED
#define UC_CONTROLLER_CLASSIC PAD_TRANSPORT_CLASSIC
#define UC_CONTROLLER_BLE PAD_TRANSPORT_BLE
#define UC_CONTROLLER_USB PAD_TRANSPORT_USB
typedef struct {
    uc_controller_transport transport;
    uint16_t vid, pid;
    unsigned has_name, has_profile, has_mapping, has_ble;
    unsigned has_ids, has_address, has_match_name;
    uint8_t address[6]; /* Bluetooth wire order, like host link addresses. */
    char match_name[64];
    int ble;
    char name[64], profile[32], mapping[1024];
} uc_controller_override;
typedef struct {
    size_t count;
    uc_controller_override devices[UC_OVERRIDE_MAX];
} uc_controller_overrides;
/* Parser is side-effect free; errors never expose a partially usable result.
 * JSON has bounded nesting, input size and token count.
 * Match requires exact transport plus VID/PID, or Classic address/name selectors.
 * All supplied selectors must match; address > name > VID/PID precedence.
 * mapping contains raw HID button/axis roles, never user inversion preferences. */
int uc_controller_json_parse(const unsigned char *data, size_t size, uc_controller_overrides *out,
                             char *error, size_t capacity);

/* Load once before opening inputs. 1 applied, 0 absent, -1 rejected; rejection
 * discards all overrides and leaves immutable builtin tables in effect. */
int uc_controller_overrides_load(const char *path, char *error, size_t capacity);
void uc_controller_overrides_clear(void);
const uc_controller_override *uc_controller_override_find(pad_transport transport, uint16_t vid,
                                                          uint16_t pid);
/* Classic identity matching: all supplied fields are ANDed. Narrow selectors
 * outrank broad rules; equally specific overlapping rules are rejected on load. */
const uc_controller_override *uc_controller_override_identity(pad_transport transport, uint16_t vid,
                                                              uint16_t pid,
                                                              const uint8_t address[6],
                                                              const char *name);
/* Whether a configured Classic name selector could match this identity.
 * Used to query a missing name even if the VID/PID already has a builtin. */
int uc_controller_override_needs_name(uint16_t vid, uint16_t pid, const uint8_t address[6]);
/* 1 means explicit profile override (NULL = generic HID), 0 means builtin. */
int uc_controller_override_profile(pad_transport transport, uint16_t vid, uint16_t pid,
                                   const void **profile);
#endif
