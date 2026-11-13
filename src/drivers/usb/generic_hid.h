#ifndef UC_USB_GENERIC_HID_H
#define UC_USB_GENERIC_HID_H
#include "../../core/pad.h"
#include <stddef.h>
#define UC_USB_HID_UNIMPLEMENTED (-2)
/* Reserved USB HID input entry point. Always returns UNIMPLEMENTED and leaves
 * state untouched. Descriptor recognition never enables input or USB output. */
int uc_usb_generic_hid_parse(const unsigned char *report, size_t length, pad_state *state);
#endif
