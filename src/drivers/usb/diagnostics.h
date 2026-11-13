#ifndef UC_USB_DIAGNOSTICS_H
#define UC_USB_DIAGNOSTICS_H
#include <stddef.h>
#include <stdint.h>
#define UC_USB_DIAG_INTERFACES 8
#define UC_USB_DIAG_ENDPOINTS 16
#define UC_USB_DIAG_CONFIG 256
#define UC_USB_DIAG_REPORT 512

typedef struct {
    uint8_t number, index, alternate, class_code, subclass, protocol, endpoints;
    uint16_t report_length;
} uc_usb_interface_info;
typedef struct {
    uint8_t interface_number, alternate, address, attributes, interval;
    uint16_t packet_size;
} uc_usb_endpoint_info;
/* Bounded diagnostic snapshot, not a driver authorization or online-device list. */
typedef struct {
    uint8_t device_class, config_index, config_value;
    unsigned config_length, config_saved, interface_count, endpoint_count;
    int config_valid, interfaces_truncated, endpoints_truncated;
    uint32_t config_hash;
    uc_usb_interface_info interfaces[UC_USB_DIAG_INTERFACES];
    uc_usb_endpoint_info endpoints[UC_USB_DIAG_ENDPOINTS];
    uint8_t config[UC_USB_DIAG_CONFIG];
    int report_interface, report_errno, report_attempted, report_valid;
    unsigned report_declared, report_saved;
    int gamepad, joystick, keyboard, mouse;
    uint8_t report[UC_USB_DIAG_REPORT];
    int detach_attempted, detached, detach_errno, claim_attempted, claimed, claim_errno;
    unsigned first_input_length, first_input_saved;
    uint8_t first_input[64];
    char summary[192];
} uc_usb_diagnostics;
/* These functions do not perform I/O or change interface ownership. */
void uc_usb_diagnose_config(uc_usb_diagnostics *, const uint8_t *, size_t);
void uc_usb_diagnose_report(uc_usb_diagnostics *, const uint8_t *, size_t);
const char *uc_usb_diagnostic_hint(uint16_t vid, uint16_t pid, const uc_usb_diagnostics *);
#endif
