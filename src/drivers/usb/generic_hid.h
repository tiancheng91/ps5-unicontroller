#ifndef UC_USB_GENERIC_HID_H
#define UC_USB_GENERIC_HID_H
#include "../protocols/profiles.h"
#include <stddef.h>
typedef struct {
    union {
        uint64_t align;
        unsigned char bytes[PROFILE_CTX_MAX];
    } context;
    pad_state latest;
    int initialized;
} uc_usb_generic_hid;
int uc_usb_generic_hid_begin(uc_usb_generic_hid *, const uint8_t *descriptor, size_t length,
                             uint16_t vid, uint16_t pid);
/* 1 normalized input, 0 unrelated Report ID, -1 malformed/truncated input.
 * Controls distributed across multiple reports retain their previous values. */
int uc_usb_generic_hid_parse(uc_usb_generic_hid *, const uint8_t *, size_t, pad_state *);
#endif
