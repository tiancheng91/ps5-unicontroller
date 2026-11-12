#ifndef UC_USB_TRANSPORT_H
#define UC_USB_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* Only Nintendo Switch Pro and wired Xbox 360/XInput interfaces in this phase. */
typedef enum { UC_USB_NONE, UC_USB_SWITCH, UC_USB_XINPUT } uc_usb_kind;
typedef struct {
    uc_usb_kind kind;
    uint16_t vid, pid;
    uint8_t iface, ep_in, ep_out;
    uint8_t iface_index; /* Descriptor-order index used by FreeBSD interface ioctls. */
    uint16_t packet_in, packet_out;
    char node[24];
} uc_usb_device;
/* Descriptor-only selection, no I/O; rejects malformed/ambiguous configurations. */
int uc_usb_select(uint16_t vid, uint16_t pid, const uint8_t *cfg, size_t length,
                  uc_usb_device *out);
/* One opened device per transport; all methods are bounded and nonblocking.
 * scan: 1 opened, 0 none, -1 fatal. read/write: bytes, 0 would-block, -1 error.
 * alive: 1 attached, 0 detached, -1 error; no reset/detach of kernel drivers.
 * close releases device/interface but transport can scan again. */
typedef struct {
    int (*scan)(void *, uc_usb_device *);
    int (*read)(void *, uint8_t *, size_t);
    int (*write)(void *, const uint8_t *, size_t);
    int (*alive)(void *);
    void (*close)(void *);
} uc_usb_ops;
typedef struct {
    const uc_usb_ops *ops;
    void *context;
} uc_usb_transport;
/* PS5 native adapter. Host builds return unsupported (0), never fake devices. */
int uc_usb_transport_open(uc_usb_transport *out);
#endif
