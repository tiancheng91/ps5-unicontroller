#ifndef UC_USB_TRANSPORT_H
#define UC_USB_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include "diagnostics.h"
/* USB currently supports wired Xbox 360-style XInput interfaces only. */
typedef enum { UC_USB_NONE, UC_USB_XINPUT, UC_USB_GENERIC_HID } uc_usb_kind;
typedef struct {
    uc_usb_kind kind;
    uint16_t vid, pid;
    uint8_t iface, ep_in, ep_out;
    uint8_t iface_index; /* Descriptor-order index used by FreeBSD interface ioctls. */
    uint16_t packet_in, packet_out;
    char node[24];
    char reason[40]; /* Static diagnostic code for a rejected candidate. */
    int error_number;
    uc_usb_diagnostics diagnostic;
} uc_usb_device;
/* Descriptor-only selection, no I/O; rejects malformed/ambiguous configurations. */
int uc_usb_select(uint16_t vid, uint16_t pid, const uint8_t *cfg, size_t length,
                  uc_usb_device *out);
/* One opened device per transport. Endpoint I/O is nonblocking; scan bounds work
 * but descriptor control ioctls depend on kernel timeout behavior.
 * scan: 1 opened, 2 rejected candidate with diagnostic, 0 none, -1 fatal. read/write: bytes, 0
 * would-block, -1 error. alive: 1 attached, 0 detached, -1 error; detach is limited to the selected
 * XInput interface; never reset devices. close releases device/interface but transport can scan
 * again. */
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
/* End a native transport lease; close() alone keeps its scan context reusable. */
void uc_usb_transport_release(uc_usb_transport *transport);
/* Check at most one cached HID node. 1 means removed/replaced: caller drops its
 * diagnostic snapshot. Uncertain access failures retain evidence. No detach. */
int uc_usb_transport_take_removed(char *node, size_t capacity);
#endif
