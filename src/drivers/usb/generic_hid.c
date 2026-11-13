#include "generic_hid.h"
#include <string.h>
int uc_usb_generic_hid_begin(uc_usb_generic_hid *hid, const uint8_t *descriptor, size_t length,
                             uint16_t vid, uint16_t pid)
{
    if (!hid)
        return 0;
    memset(hid, 0, sizeof *hid);
    if (!descriptor || !length || length > 512 ||
        !generic_setup_for_transport(hid->context.bytes, descriptor, (int)length, vid, pid,
                                     PAD_TRANSPORT_USB))
        return 0;
    pad_state_reset(&hid->latest);
    hid->initialized = 1;
    return 1;
}
int uc_usb_generic_hid_parse(uc_usb_generic_hid *hid, const uint8_t *report, size_t length,
                             pad_state *out)
{
    if (!hid || !hid->initialized || !report || !out || !length || length > 512)
        return -1;
    int expected = generic_input_report_bytes(hid->context.bytes, report[0]);
    if (!expected)
        return 0;
    if (length < (size_t)expected)
        return -1;
    pad_state next = hid->latest;
    if (generic_profile.parse(hid->context.bytes, report, (int)length, &next) == PARSE_NONE)
        return 0;
    hid->latest = *out = next;
    return 1;
}
