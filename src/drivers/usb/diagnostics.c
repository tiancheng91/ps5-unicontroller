#include "diagnostics.h"
#include <stdio.h>
#include <string.h>
static unsigned word(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}
void uc_usb_diagnose_config(uc_usb_diagnostics *d, const uint8_t *cfg, size_t length)
{
    memset(d, 0, sizeof(*d));
    d->report_interface = -1;
    d->config_length = (unsigned)length;
    d->config_saved = length < sizeof(d->config) ? (unsigned)length : sizeof(d->config);
    if (!cfg) {
        d->config_saved = 0;
        return;
    }
    memcpy(d->config, cfg, d->config_saved);
    d->config_hash = 2166136261u;
    for (size_t i = 0; i < length; i++)
        d->config_hash = (d->config_hash ^ cfg[i]) * 16777619u;
    if (length < 9 || cfg[0] < 9 || cfg[0] > length || cfg[1] != 2 || word(cfg + 2) != length)
        return;
    d->config_value = cfg[5];
    int current = -1;
    unsigned index = 0;
    size_t text = 0;
    for (size_t offset = cfg[0]; offset < length; offset += cfg[offset]) {
        const uint8_t *p = cfg + offset;
        if (length - offset < 2 || p[0] < 2 || p[0] > length - offset)
            return;
        if (p[1] == 4) {
            if (p[0] < 9)
                return;
            current = -1;
            if (d->interface_count < UC_USB_DIAG_INTERFACES) {
                current = (int)d->interface_count++;
                uc_usb_interface_info *i = &d->interfaces[current];
                *i = (uc_usb_interface_info){.number = p[2],
                                             .index = (uint8_t)index,
                                             .alternate = p[3],
                                             .endpoints = p[4],
                                             .class_code = p[5],
                                             .subclass = p[6],
                                             .protocol = p[7]};
                if (text < sizeof(d->summary)) {
                    int n = snprintf(d->summary + text, sizeof(d->summary) - text,
                                     "%sIF%u alt%u %02X/%02X/%02X", text ? "; " : "", p[2], p[3],
                                     p[5], p[6], p[7]);
                    if (n > 0)
                        text += (size_t)n;
                }
            } else {
                d->interfaces_truncated = 1;
            }
            if (p[3] == 0)
                ++index;
        } else if (p[1] == 0x21 && current >= 0 && d->interfaces[current].class_code == 3) {
            if (p[0] < 6 || p[5] > (p[0] - 6) / 3)
                return;
            for (unsigned i = 0; i < p[5]; i++)
                if (p[6 + i * 3] == 0x22)
                    d->interfaces[current].report_length = (uint16_t)word(p + 7 + i * 3);
        } else if (p[1] == 5) {
            if (p[0] < 7 || current < 0) {
                if (d->interfaces_truncated)
                    continue;
                return;
            }
            if (d->endpoint_count < UC_USB_DIAG_ENDPOINTS) {
                uc_usb_endpoint_info *e = &d->endpoints[d->endpoint_count++];
                *e = (uc_usb_endpoint_info){.interface_number = d->interfaces[current].number,
                                            .alternate = d->interfaces[current].alternate,
                                            .address = p[2],
                                            .attributes = p[3],
                                            .packet_size = (uint16_t)word(p + 4),
                                            .interval = p[6]};
            } else {
                d->endpoints_truncated = 1;
            }
        }
    }
    d->config_valid = 1;
}
void uc_usb_diagnose_report(uc_usb_diagnostics *d, const uint8_t *bytes, size_t length)
{
    d->report_valid = d->gamepad = d->joystick = d->keyboard = d->mouse = 0;
    d->report_saved = 0;
    memset(d->report, 0, sizeof(d->report));
    if (!bytes || !length || length > sizeof(d->report))
        return;
    memcpy(d->report, bytes, length);
    d->report_saved = (unsigned)length;
    /* A partial descriptor is useful evidence, never proof of a gamepad. */
    if (d->report_declared != length)
        return;
    uint32_t page = 0, usage = 0, stack[4];
    unsigned globals = 0, collections = 0;
    int gamepad = 0, joystick = 0, keyboard = 0, mouse = 0;
    for (size_t at = 0; at < length;) {
        uint8_t prefix = bytes[at++];
        if (prefix == 0xfe) {
            if (length - at < 2 || bytes[at] > length - at - 2)
                return;
            at += 2 + bytes[at];
            continue;
        }
        unsigned size = (prefix & 3) == 3 ? 4 : prefix & 3;
        if (size > length - at)
            return;
        uint32_t value = 0;
        for (unsigned i = 0; i < size; i++)
            value |= (uint32_t)bytes[at++] << (8 * i);
        unsigned type = prefix >> 2 & 3, tag = prefix >> 4;
        if (type == 1 && tag == 0) {
            if (value > 0xffff)
                return;
            page = value;
        } else if (type == 1 && tag == 10) {
            if (globals == 4)
                return;
            stack[globals++] = page;
        } else if (type == 1 && tag == 11) {
            if (!globals)
                return;
            page = stack[--globals];
        } else if (type == 2 && tag == 0)
            usage = size == 4 ? value : (page << 16) | value;
        else if (type == 0) {
            if (tag == 10) {
                if (!collections && value == 1) {
                    joystick |= usage == 0x00010004;
                    gamepad |= usage == 0x00010005;
                    keyboard |= usage == 0x00010006;
                    mouse |= usage == 0x00010002;
                }
                if (++collections > 32)
                    return;
            } else if (tag == 12) {
                if (!collections)
                    return;
                --collections;
            }
            usage = 0;
        }
    }
    if (collections || globals)
        return;
    d->report_valid = 1;
    d->gamepad = gamepad;
    d->joystick = joystick;
    d->keyboard = keyboard;
    d->mouse = mouse;
}
const char *uc_usb_diagnostic_hint(uint16_t vid, uint16_t pid, const uc_usb_diagnostics *d)
{
    if (vid == 0x057e && pid == 0x2009)
        return "switch-pro";
    if (vid == 0x054c)
        return "system-sony";
    if (d->config_valid)
        for (unsigned i = 0; i < d->interface_count; i++) {
            const uc_usb_interface_info *p = &d->interfaces[i];
            if (!p->alternate && p->class_code == 0xff && p->subclass == 0x5d && p->protocol == 1)
                return "xinput-candidate";
        }
    if (d->report_valid && (d->gamepad || d->joystick))
        return d->keyboard || d->mouse ? "hid-mixed" : "generic-hid-gamepad";
    if (d->report_valid && d->keyboard)
        return "hid-keyboard";
    if (d->report_valid && d->mouse)
        return "hid-mouse";
    for (unsigned i = 0; i < d->interface_count; i++)
        if (d->interfaces[i].class_code == 3)
            return "hid-unverified";
    return "unknown";
}
