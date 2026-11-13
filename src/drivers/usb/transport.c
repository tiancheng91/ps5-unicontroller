#include "transport.h"
#include "../protocols/profiles.h"
#include <string.h>
static unsigned le16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}
/* Selection never changes configuration/alternate setting. Only active alt 0
 * interfaces with exactly one interrupt IN/OUT pair are accepted. */
int uc_usb_select(uint16_t vid, uint16_t pid, const uint8_t *cfg, size_t length, uc_usb_device *out)
{
    if (!cfg || !out || length < 9 || cfg[0] < 9 || cfg[1] != 2 || le16(cfg + 2) != length ||
        vid == 0x054c || vid == 0x0e8d || cfg[4] == 0)
        return 0;
    int hint = profile_usb_protocol_hint(vid, pid);
    int switch_identity = hint == 1 || (!hint && vid == 0x057e && pid == 0x2009);
    uc_usb_device candidate = {0}, current = {0};
    unsigned count = 0, declared = 0, endpoints = 0, seen_interfaces = 0,
             expected_interfaces = cfg[4];
    uint8_t numbers[256] = {0};
    int active = 0, wanted = 0, have_interface = 0;
    for (size_t offset = cfg[0]; offset <= length;) {
        int at_end = offset == length;
        if (!at_end && (length - offset < 2 || cfg[offset] < 2 || cfg[offset] > length - offset))
            return 0;
        const uint8_t *d = at_end ? NULL : cfg + offset;
        if (at_end || d[1] == 4) {
            if (have_interface && endpoints != declared)
                return 0;
            if (wanted) {
                if (!current.ep_in || !current.ep_out || endpoints != 2)
                    return 0;
                candidate = current;
                if (++count > 1)
                    return 0;
            }
            if (at_end)
                break;
            if (d[0] < 9)
                return 0;
            memset(&current, 0, sizeof(current));
            current.vid = vid;
            current.pid = pid;
            declared = d[4];
            endpoints = 0;
            wanted = 0;
            have_interface = 1;
            active = d[3] == 0;
            if (active) {
                if (numbers[d[2]])
                    return 0;
                numbers[d[2]] = 1;
                current.iface_index = (uint8_t)seen_interfaces++;
            }
            current.iface = d[2];
            if (active && switch_identity && d[5] == 3 && d[6] == 0 && d[7] == 0) {
                current.kind = UC_USB_SWITCH;
                wanted = 1;
            } else if (active && !switch_identity && d[5] == 0xff && d[6] == 0x5d && d[7] == 1) {
                current.kind = UC_USB_XINPUT;
                wanted = 1;
            }
        } else if (d[1] == 5) {
            if (!have_interface || d[0] < 7 || !(d[2] & 15) || (d[2] & 0x70))
                return 0;
            ++endpoints;
            if (wanted) {
                unsigned packet = le16(d + 4);
                unsigned minimum = current.kind == UC_USB_SWITCH ? 64 : 20;
                if ((d[3] & 3) != 3 || packet < minimum || packet > 64 || !d[6])
                    return 0;
                if (d[2] & 0x80) {
                    if (current.ep_in)
                        return 0;
                    current.ep_in = d[2];
                    current.packet_in = (uint16_t)packet;
                } else {
                    if (current.ep_out)
                        return 0;
                    current.ep_out = d[2];
                    current.packet_out = (uint16_t)packet;
                }
            }
        }
        offset += d[0];
    }
    if (seen_interfaces != expected_interfaces || count != 1)
        return 0;
    *out = candidate;
    return 1;
}
#ifdef UC_PS5
#include <sys/types.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../common/log.h"
/* FreeBSD usb_fs gives asynchronous completions without libusb. Exactly two
 * transfer slots are opened, for the selected interface's endpoints only.
 * Unsupported ownership ioctls fail closed; no detach/reset/configuration write. */
typedef struct {
    int fd, claimed, initialized, busy[2], opened[2], dead, leased, output_dead;
    int iface_index;
    unsigned diagnostic_mask, scan_nodes;
    uint16_t observed_nodes[16];
    unsigned observed_count;
    DIR *directory;
    uc_usb_device device;
    struct usb_fs_endpoint endpoints[2];
    uint8_t buffers[2][64];
    void *pointers[2];
    uint32_t lengths[2], requested[2];
    uint8_t received[64];
    unsigned received_length;
} native_transport;
static native_transport natives[4];
static void native_close(void *context)
{
    native_transport *n = context;
    if (n->fd >= 0) {
        if (n->initialized) {
            for (unsigned i = 0; i < 2; i++)
                if (n->opened[i]) {
                    struct usb_fs_close close_request = {(uint8_t)i};
                    (void)ioctl(n->fd, USB_FS_CLOSE, &close_request);
                }
            struct usb_fs_uninit uninit = {0};
            (void)ioctl(n->fd, USB_FS_UNINIT, &uninit);
        }
        if (n->claimed)
            (void)ioctl(n->fd, USB_RELEASE_INTERFACE, &n->iface_index);
        close(n->fd);
    }
    if (n->directory) {
        closedir(n->directory);
        n->directory = NULL;
    }
    n->fd = -1;
    n->claimed = n->initialized = n->dead = n->output_dead = 0;
    n->received_length = 0;
    memset(n->opened, 0, sizeof(n->opened));
    memset(n->busy, 0, sizeof(n->busy));
}
static int start(native_transport *n, unsigned slot, unsigned length)
{
    n->lengths[slot] = n->requested[slot] = length;
    n->endpoints[slot].nFrames = 1;
    n->endpoints[slot].aFrames = 0;
    n->endpoints[slot].status = 0;
    struct usb_fs_start request = {(uint8_t)slot};
    if (ioctl(n->fd, USB_FS_START, &request) < 0) {
        if (slot == 1)
            n->output_dead = 1;
        else
            n->dead = 1;
        return -1;
    }
    n->busy[slot] = 1;
    return 0;
}
static int pump(native_transport *n)
{
    for (unsigned round = 0; round < 2; round++) {
        struct usb_fs_complete completion = {0};
        if (ioctl(n->fd, USB_FS_COMPLETE, &completion) < 0) {
            if (errno == EAGAIN || errno == EBUSY)
                return 0;
            n->dead = 1;
            return -1;
        }
        unsigned slot = completion.ep_index;
        if (slot >= 2 || !n->busy[slot]) {
            n->dead = 1;
            return -1;
        }
        n->busy[slot] = 0;
        if (n->endpoints[slot].status || n->lengths[slot] > 64 ||
            (slot == 1 && n->lengths[slot] != n->requested[slot])) {
            if (slot == 1) {
                n->output_dead = 1;
                continue;
            }
            n->dead = 1;
            return -1;
        }
        if (slot == 0 && n->endpoints[slot].aFrames && n->lengths[0]) {
            if (n->received_length) {
                n->dead = 1;
                return -1;
            }
            n->received_length = n->lengths[0];
            memcpy(n->received, n->buffers[0], n->received_length);
        } else if (slot == 0) {
            /* Empty successful interrupt completions are not detach events. */
            if (start(n, 0, n->device.packet_in) < 0)
                return -1;
        }
    }
    return 0;
}
static int open_candidate(native_transport *n, const char *node, uc_usb_device *selected)
{
    unsigned diagnostic = 0;
    const char *operation = "device_descriptor";
    for (unsigned i = 0; i < 4; i++)
        if (&natives[i] != n && natives[i].leased && natives[i].fd >= 0 &&
            !strcmp(natives[i].device.node, node))
            return 0;
    n->fd = open(node, O_RDWR | O_NONBLOCK);
    if (n->fd < 0) {
        if (!(n->diagnostic_mask & 1u)) {
            n->diagnostic_mask |= 1u;
            log_line("usb %s: open failed errno=%d", node, errno);
        }
        return 0;
    }
    struct usb_device_descriptor descriptor;
    struct usb_device_info info;
    uint8_t config[4096];
    memset(&info, 0, sizeof(info));
    if (ioctl(n->fd, USB_GET_DEVICE_DESC, &descriptor) < 0 ||
        ioctl(n->fd, USB_GET_DEVICEINFO, &info) < 0)
        goto reject;
    unsigned bus = 0, address = 0;
    (void)sscanf(node, "/dev/ugen%u.%u", &bus, &address);
    uint16_t identity = (uint16_t)(bus << 8 | address);
    int observed = 0;
    for (unsigned i = 0; i < n->observed_count; i++)
        observed |= n->observed_nodes[i] == identity;
    if (!observed && n->observed_count < 16) {
        n->observed_nodes[n->observed_count++] = identity;
        log_line("usb %s: enumerated vid=%04x pid=%04x class=%02x config=%u", node,
                 UGETW(descriptor.idVendor), UGETW(descriptor.idProduct), descriptor.bDeviceClass,
                 info.udi_config_index);
    }
    operation = "device_class_filter";
    if (descriptor.bDeviceClass != 0 && descriptor.bDeviceClass != 0xff) {
        errno = 0;
        goto reject;
    }
    struct usb_gen_descriptor full;
    memset(&full, 0, sizeof(full));
    full.ugd_data = config;
    full.ugd_maxlen = sizeof(config);
    full.ugd_config_index = info.udi_config_index;
    operation = "configuration_descriptor";
    if (ioctl(n->fd, USB_GET_FULL_DESC, &full) < 0)
        goto reject;
    operation = "protocol_or_endpoint_filter";
    diagnostic = 6;
    errno = 0;
    if (full.ugd_actlen > sizeof(config) ||
        !uc_usb_select(UGETW(descriptor.idVendor), UGETW(descriptor.idProduct), config,
                       full.ugd_actlen, selected))
        goto reject;
    n->iface_index = selected->iface_index;
    diagnostic = 1;
    operation = "driver_active";
    /* ACTIVE succeeds when a kernel driver owns the interface. ENXIO is the
     * documented no-driver result. Any other response is unsafe/unsupported. */
    if (ioctl(n->fd, USB_IFACE_DRIVER_ACTIVE, &n->iface_index) == 0 || errno != ENXIO)
        goto reject;
    diagnostic = 2;
    operation = "claim_interface";
    if (ioctl(n->fd, USB_CLAIM_INTERFACE, &n->iface_index) < 0)
        goto reject;
    n->claimed = 1;
    diagnostic = 3;
    operation = "active_alternate";
    struct usb_alt_interface alternate;
    memset(&alternate, 0, sizeof(alternate));
    alternate.uai_interface_index = selected->iface_index;
    if (ioctl(n->fd, USB_GET_ALTINTERFACE, &alternate) < 0 || alternate.uai_alt_index != 0)
        goto reject;
    memset(n->endpoints, 0, sizeof(n->endpoints));
    for (unsigned i = 0; i < 2; i++) {
        n->pointers[i] = n->buffers[i];
        n->endpoints[i].ppBuffer = &n->pointers[i];
        n->endpoints[i].pLength = &n->lengths[i];
        n->endpoints[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        n->endpoints[i].timeout = i ? 250 : USB_FS_TIMEOUT_NONE;
    }
    diagnostic = 4;
    operation = "transfer_init";
    struct usb_fs_init init = {.pEndpoints = n->endpoints, .ep_index_max = 2};
    if (ioctl(n->fd, USB_FS_INIT, &init) < 0)
        goto reject;
    n->initialized = 1;
    diagnostic = 5;
    operation = "endpoint_open";
    for (unsigned i = 0; i < 2; i++) {
        struct usb_fs_open endpoint;
        memset(&endpoint, 0, sizeof(endpoint));
        endpoint.ep_index = (uint8_t)i;
        endpoint.ep_no = i ? selected->ep_out : selected->ep_in;
        endpoint.max_bufsize = 64;
        endpoint.max_frames = 1;
        if (ioctl(n->fd, USB_FS_OPEN, &endpoint) < 0)
            goto reject;
        n->opened[i] = 1;
    }
    if (start(n, 0, selected->packet_in) < 0)
        goto reject;
    snprintf(selected->node, sizeof(selected->node), "%s", node);
    n->device = *selected;
    return 1;
reject: {
    /* Repeated enumeration must not flood logs; one diagnostic per stage. */
    if (!(n->diagnostic_mask & (1u << diagnostic))) {
        n->diagnostic_mask |= 1u << diagnostic;
        log_line("usb %s: rejected at %s errno=%d (no detach/reset attempted)", node, operation,
                 errno);
    }
    DIR *directory = n->directory;
    n->directory = NULL;
    native_close(n);
    n->directory = directory;
}
    return 0;
}
static int native_scan(void *context, uc_usb_device *out)
{
    native_transport *n = context;
    if (n->fd >= 0 || !out)
        return -1;
    if (!n->directory)
        n->directory = opendir("/dev");
    if (!n->directory) {
        if (!(n->diagnostic_mask & (1u << 7))) {
            n->diagnostic_mask |= 1u << 7;
            log_line("usb: opendir /dev failed errno=%d", errno);
        }
        return -1;
    }
    /* At most 128 directory entries and one candidate open/descriptor query per
     * scan. Opening ugen control nodes reads descriptors, never all endpoints. */
    for (unsigned examined = 0; examined < 128; examined++) {
        struct dirent *entry = readdir(n->directory);
        if (!entry) {
            if (!(n->diagnostic_mask & (1u << 8))) {
                n->diagnostic_mask |= 1u << 8;
                log_line("usb: /dev scan completed ugen_candidates=%u", n->scan_nodes);
            }
            n->scan_nodes = 0;
            closedir(n->directory);
            n->directory = NULL;
            return 0;
        }
        unsigned bus, address;
        char trailing;
        if (sscanf(entry->d_name, "ugen%u.%u%c", &bus, &address, &trailing) != 2 || bus > 255 ||
            address > 127 || !address)
            continue;
        ++n->scan_nodes;
        char node[24];
        int length = snprintf(node, sizeof(node), "/dev/ugen%u.%u", bus, address);
        if (length < 0 || (size_t)length >= sizeof(node))
            continue;
        return open_candidate(n, node, out);
    }
    return 0;
}
static int native_read(void *context, uint8_t *buffer, size_t capacity)
{
    native_transport *n = context;
    if (n->fd < 0 || n->dead || !buffer || pump(n) < 0)
        return -1;
    if (!n->received_length)
        return 0;
    if (capacity < n->received_length) {
        n->dead = 1;
        return -1;
    }
    unsigned length = n->received_length;
    memcpy(buffer, n->received, length);
    n->received_length = 0;
    if (start(n, 0, n->device.packet_in) < 0)
        return -1;
    return (int)length;
}
static int native_write(void *context, const uint8_t *buffer, size_t length)
{
    native_transport *n = context;
    if (n->fd < 0 || n->dead || n->output_dead || !buffer || !length || length > 64 ||
        pump(n) < 0 || n->output_dead)
        return -1;
    if (n->busy[1])
        return 0;
    memcpy(n->buffers[1], buffer, length);
    if (start(n, 1, (unsigned)length) < 0)
        return -1;
    /* Success means queued; a later completion failure is surfaced by read,
     * write or alive. Protocol ACKs prove delivery of initialization commands. */
    return (int)length;
}
static int native_alive(void *context)
{
    native_transport *n = context;
    if (n->fd < 0 || n->dead)
        return 0;
    if (pump(n) < 0)
        return -1;
    struct usb_device_info info;
    memset(&info, 0, sizeof(info));
    if (ioctl(n->fd, USB_GET_DEVICEINFO, &info) < 0)
        return errno == ENXIO || errno == ENODEV ? 0 : -1;
    if (info.udi_vendorNo != n->device.vid || info.udi_productNo != n->device.pid)
        return 0;
    return 1;
}
static const uc_usb_ops ops = {native_scan, native_read, native_write, native_alive, native_close};
int uc_usb_transport_open(uc_usb_transport *out)
{
    if (!out)
        return 0;
    for (unsigned i = 0; i < 4; i++) {
        native_transport *n = &natives[i];
        if (n->leased)
            continue;
        memset(n, 0, sizeof(*n));
        n->fd = -1;
        n->leased = 1;
        out->ops = &ops;
        out->context = n;
        return 1;
    }
    memset(out, 0, sizeof(*out));
    return 0;
}
void uc_usb_transport_release(uc_usb_transport *transport)
{
    if (!transport || transport->ops != &ops)
        return;
    for (unsigned i = 0; i < 4; i++)
        if (transport->context == &natives[i] && natives[i].leased) {
            native_close(&natives[i]);
            natives[i].leased = 0;
            break;
        }
    memset(transport, 0, sizeof(*transport));
}
#else
int uc_usb_transport_open(uc_usb_transport *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    return 0;
}
void uc_usb_transport_release(uc_usb_transport *transport)
{
    if (transport)
        memset(transport, 0, sizeof(*transport));
}
#endif
