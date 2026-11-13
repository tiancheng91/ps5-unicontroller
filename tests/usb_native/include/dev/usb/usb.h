#ifndef UC_TEST_USB_H
#define UC_TEST_USB_H
#include <stdint.h>
/* Host-only syscall fixtures, not PS5 ABI definitions. */
struct usb_device_descriptor {
    uint8_t bDeviceClass;
    uint8_t idVendor[2], idProduct[2];
};
#define UGETW(p) ((unsigned)(p)[0] | (unsigned)(p)[1] << 8)
#define USETW(p, v)                                                                                \
    do {                                                                                           \
        (p)[0] = (uint8_t)(v);                                                                     \
        (p)[1] = (uint8_t)((v) >> 8);                                                              \
    } while (0)
struct usb_device_request {
    uint8_t bmRequestType, bRequest, wValue[2], wIndex[2], wLength[2];
};
#endif
