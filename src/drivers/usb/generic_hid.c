#include "generic_hid.h"
int uc_usb_generic_hid_parse(const unsigned char *report, size_t length, pad_state *state)
{
    (void)report;
    (void)length;
    (void)state;
    return UC_USB_HID_UNIMPLEMENTED;
}
