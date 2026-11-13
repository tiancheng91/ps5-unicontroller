#include "transport.h"
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
            if (active && d[5] == 0xff && d[6] == 0x5d && d[7] == 1) {
                current.kind = UC_USB_XINPUT;
                wanted = 1;
            }
        } else if (d[1] == 5) {
            if (!have_interface || d[0] < 7 || !(d[2] & 15) || (d[2] & 0x70))
                return 0;
            ++endpoints;
            if (wanted) {
                unsigned packet = le16(d + 4);
                unsigned minimum = 20;
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
int uc_usb_select_generic(const uc_usb_device *candidate, uc_usb_device *out)
{
    if (!candidate || !out || candidate->vid == 0x054c || candidate->vid == 0x057e ||
        candidate->vid == 0x0e8d)
        return 0;
    const uc_usb_diagnostics *d = &candidate->diagnostic;
    if (!d->config_valid || d->interface_count > UC_USB_DIAG_INTERFACES ||
        d->endpoint_count > UC_USB_DIAG_ENDPOINTS || d->interfaces_truncated ||
        d->endpoints_truncated || !d->report_valid || !d->report_saved ||
        d->report_saved != d->report_declared || !(d->gamepad || d->joystick) || d->keyboard ||
        d->mouse)
        return 0;
    uc_usb_device selected = *candidate;
    selected.kind = UC_USB_GENERIC_HID;
    selected.ep_in = selected.ep_out = 0;
    selected.packet_in = selected.packet_out = 0;
    unsigned matches = 0, endpoints = 0;
    for (unsigned i = 0; i < d->interface_count; i++) {
        const uc_usb_interface_info *iface = &d->interfaces[i];
        if (iface->number != d->report_interface || iface->alternate)
            continue;
        if (++matches > 1 || iface->class_code != 3 || iface->subclass == 1 || iface->protocol)
            return 0;
        selected.iface = iface->number;
        selected.iface_index = iface->index;
        endpoints = iface->endpoints;
    }
    if (matches != 1)
        return 0;
    unsigned seen = 0;
    for (unsigned i = 0; i < d->endpoint_count; i++) {
        const uc_usb_endpoint_info *ep = &d->endpoints[i];
        if (ep->interface_number != selected.iface || ep->alternate)
            continue;
        if (++seen > endpoints || (ep->attributes & 3) != 3 || !ep->interval ||
            !(ep->address & 15) || (ep->address & 0x70) || !ep->packet_size ||
            ep->packet_size > 512)
            return 0;
        if (ep->address & 0x80) {
            if (selected.ep_in)
                return 0;
            selected.ep_in = ep->address;
            selected.packet_in = ep->packet_size;
        }
    }
    if (seen != endpoints || !selected.ep_in)
        return 0;
    *out = selected;
    return 1;
}
#ifdef UC_PS5
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "../common/log.h"

typedef struct {
    int fd, in_fd, out_fd, claimed, detached, leased, dead, output_dead;
    int iface_index;
    unsigned reads, diagnostic_mask;
    DIR *directory;
    uc_usb_device device;
} native_transport;
static native_transport natives[4];
/* Shared across four scan slots: one read-only descriptor request per observed
 * connection/configuration identity, including failed probes. */
typedef struct {
    dev_t device;
    ino_t inode;
    uint32_t plugtime;
    int have_inode, have_plugtime;
} hid_incarnation;
static struct {
    char node[24];
    uint16_t vid, pid;
    hid_incarnation incarnation;
    uc_usb_diagnostics diagnostic;
} hid_cache[16];
static unsigned hid_cache_count, hid_cache_next, hid_presence_next;
static hid_incarnation hid_identity(int fd)
{
    hid_incarnation result = {0};
    struct stat status;
    if (fstat(fd, &status) == 0) {
        result.have_inode = 1;
        result.device = status.st_dev;
        result.inode = status.st_ino;
    }
#ifdef USB_GET_PLUGTIME
    if (ioctl(fd, USB_GET_PLUGTIME, &result.plugtime) == 0)
        result.have_plugtime = 1;
#endif
    return result;
}
static int hid_replaced(const hid_incarnation *a, const hid_incarnation *b)
{
    return (a->have_inode && b->have_inode && (a->device != b->device || a->inode != b->inode)) ||
           (a->have_plugtime && b->have_plugtime && a->plugtime != b->plugtime);
}
static void hid_forget(const char *node)
{
    for (unsigned i = 0; i < hid_cache_count;) {
        if (strcmp(hid_cache[i].node, node)) {
            ++i;
            continue;
        }
        memmove(hid_cache + i, hid_cache + i + 1, (hid_cache_count - i - 1) * sizeof(hid_cache[0]));
        --hid_cache_count;
    }
}
int uc_usb_transport_take_removed(char *node, size_t capacity)
{
    if (!node || capacity < sizeof(hid_cache[0].node) || !hid_cache_count)
        return 0;
    unsigned i = hid_presence_next++ % hid_cache_count;
    char current[24], control[64];
    snprintf(current, sizeof(current), "%s", hid_cache[i].node);
    unsigned bus, address;
    if (sscanf(current, "/dev/ugen%u.%u", &bus, &address) != 2)
        return 0;
    snprintf(control, sizeof(control), "/dev/usb/%u.%u.0", bus, address);
    int fd = open(control, O_RDONLY | O_NONBLOCK);
    int gone = 0;
    if (fd < 0) {
        gone = errno == ENOENT || errno == ENXIO || errno == ENODEV;
    } else {
        hid_incarnation identity = hid_identity(fd);
        gone = hid_replaced(&hid_cache[i].incarnation, &identity);
        close(fd);
    }
    if (!gone)
        return 0;
    hid_forget(current);
    snprintf(node, capacity, "%s", current);
    log_line("usb %s: HID removed or replaced; descriptor cache cleared", current);
    return 1;
}

static void diagnose_hid(native_transport *n, uc_usb_device *device)
{
    uc_usb_diagnostics *d = &device->diagnostic;
    if (!d->config_valid)
        return;
    int target = -1;
    for (unsigned i = 0; i < d->interface_count; i++) {
        const uc_usb_interface_info *p = &d->interfaces[i];
        if (!p->alternate && p->class_code == 3 && p->subclass != 1 && p->report_length) {
            target = (int)i;
            break;
        }
    }
    if (target < 0)
        return;
    hid_incarnation identity = hid_identity(n->fd);
    for (unsigned i = 0; i < hid_cache_count; i++)
        if (!strcmp(hid_cache[i].node, device->node) &&
            hid_replaced(&hid_cache[i].incarnation, &identity)) {
            hid_forget(device->node);
            log_line("usb %s: HID connection incarnation changed; reading descriptor again",
                     device->node);
            break;
        }
    for (unsigned i = 0; i < hid_cache_count; i++)
        if (!strcmp(hid_cache[i].node, device->node) && hid_cache[i].vid == device->vid &&
            hid_cache[i].pid == device->pid &&
            hid_cache[i].diagnostic.config_hash == d->config_hash &&
            hid_cache[i].diagnostic.config_index == d->config_index) {
            *d = hid_cache[i].diagnostic;
            return;
        }
    d->report_interface = d->interfaces[target].number;
    d->report_declared = d->interfaces[target].report_length;
    d->report_attempted = 1;
    unsigned length =
        d->report_declared < sizeof(d->report) ? d->report_declared : sizeof(d->report);
    struct usb_ctl_request request;
    uint8_t bytes[UC_USB_DIAG_REPORT];
    memset(&request, 0, sizeof(request));
    request.ucr_data = bytes;
    request.ucr_request.bmRequestType = 0x81; /* Standard device-to-host, interface. */
    request.ucr_request.bRequest = 6;         /* GET_DESCRIPTOR, never SET_REPORT. */
    USETW(request.ucr_request.wValue, 0x2200);
    USETW(request.ucr_request.wIndex, d->report_interface);
    USETW(request.ucr_request.wLength, length);
    if (ioctl(n->fd, USB_DO_REQUEST, &request) < 0) {
        d->report_errno = errno;
    } else if (request.ucr_actlen > length) {
        d->report_errno = EOVERFLOW;
    } else {
        uc_usb_diagnose_report(d, bytes, request.ucr_actlen);
    }
    log_line("usb %s: HID descriptor interface=%d declared=%u captured=%u valid=%d "
             "gamepad=%d joystick=%d keyboard=%d mouse=%d errno=%d",
             device->node, d->report_interface, d->report_declared, d->report_saved,
             d->report_valid, d->gamepad, d->joystick, d->keyboard, d->mouse, d->report_errno);
    unsigned index = hid_cache_count < 16 ? hid_cache_count++ : hid_cache_next++ % 16;
    snprintf(hid_cache[index].node, sizeof(hid_cache[index].node), "%s", device->node);
    hid_cache[index].vid = device->vid;
    hid_cache[index].pid = device->pid;
    hid_cache[index].diagnostic = *d;
    hid_cache[index].incarnation = identity;
}

static void native_close(void *context)
{
    native_transport *n = context;
    if (n->in_fd >= 0)
        close(n->in_fd);
    if (n->out_fd >= 0)
        close(n->out_fd);
    if (n->fd >= 0) {
        if (n->claimed && ioctl(n->fd, USB_RELEASE_INTERFACE, &n->iface_index) < 0)
            log_line("usb %s: interface release failed errno=%d", n->device.node, errno);
        if (n->detached)
            log_line("usb %s: detached interface closed; replug to restore system driver if needed",
                     n->device.node);
        close(n->fd);
    }
    if (n->directory)
        closedir(n->directory);
    n->directory = NULL;
    n->fd = n->in_fd = n->out_fd = -1;
    n->claimed = n->detached = n->dead = n->output_dead = 0;
    n->reads = 0;
}

static int open_candidate(native_transport *n, unsigned bus, unsigned address, uc_usb_device *out)
{
    char path[64];
    uc_usb_device selected = {0};
    selected.diagnostic.report_interface = -1;
    snprintf(selected.node, sizeof(selected.node), "/dev/ugen%u.%u", bus, address);
    for (unsigned i = 0; i < 4; i++)
        if (&natives[i] != n && natives[i].leased && natives[i].fd >= 0 &&
            !strcmp(natives[i].device.node, selected.node))
            return 0;
    const char *stage = "control_open";
    int report = 1;
    snprintf(path, sizeof(path), "/dev/usb/%u.%u.0", bus, address);
    n->fd = open(path, O_RDWR | O_NONBLOCK);
    if (n->fd < 0)
        goto reject;
    struct usb_device_descriptor descriptor;
    struct usb_device_info info;
    memset(&descriptor, 0, sizeof(descriptor));
    memset(&info, 0, sizeof(info));
    stage = "device_descriptor";
    selected.diagnostic.descriptor_attempted = 1;
    if (ioctl(n->fd, USB_GET_DEVICE_DESC, &descriptor) < 0) {
        selected.diagnostic.descriptor_errno = errno;
        goto reject;
    }
    selected.diagnostic.descriptor_ok = 1;
    selected.vid = UGETW(descriptor.idVendor);
    selected.pid = UGETW(descriptor.idProduct);
    selected.diagnostic.device_class = descriptor.bDeviceClass;
    stage = "device_info";
    selected.diagnostic.info_attempted = 1;
    if (ioctl(n->fd, USB_GET_DEVICEINFO, &info) < 0) {
        selected.diagnostic.info_errno = errno;
        goto reject;
    }
    selected.diagnostic.info_ok = 1;
    selected.diagnostic.config_index = info.udi_config_index;
    /* Leave Sony controllers and the internal Bluetooth vendor to the system.
     * Device class alone never authorizes detaching a driver. */
    if (selected.vid == 0x054c) {
        stage = "system_device_excluded";
        errno = 0;
        goto reject;
    }
    if (selected.vid == 0x0e8d ||
        (descriptor.bDeviceClass != 0 && descriptor.bDeviceClass != 0xff &&
         descriptor.bDeviceClass != 0xef && descriptor.bDeviceClass != 3)) {
        report = 0;
        goto reject;
    }
    uint8_t config[4096];
    struct usb_gen_descriptor full;
    memset(&full, 0, sizeof(full));
    full.ugd_data = config;
    full.ugd_maxlen = sizeof(config);
    full.ugd_config_index = info.udi_config_index;
    stage = "configuration_descriptor";
    if (ioctl(n->fd, USB_GET_FULL_DESC, &full) < 0)
        goto reject;
    if (full.ugd_actlen > sizeof(config)) {
        errno = EOVERFLOW;
        goto reject;
    }
    uc_usb_diagnose_config(&selected.diagnostic, config, full.ugd_actlen);
    selected.diagnostic.descriptor_attempted = selected.diagnostic.descriptor_ok = 1;
    selected.diagnostic.info_attempted = selected.diagnostic.info_ok = 1;
    selected.diagnostic.device_class = descriptor.bDeviceClass;
    selected.diagnostic.config_index = info.udi_config_index;
    diagnose_hid(n, &selected);
    uc_usb_device matched = {0};
    stage = "not_supported_xinput";
    errno = 0;
    if (!uc_usb_select(selected.vid, selected.pid, config, full.ugd_actlen, &matched) &&
        !uc_usb_select_generic(&selected, &matched)) {
        const char *hint = uc_usb_diagnostic_hint(selected.vid, selected.pid, &selected.diagnostic);
        if (!strcmp(hint, "generic-hid-gamepad")) {
            selected.kind = UC_USB_GENERIC_HID;
            stage = "generic_hid_interface_unsupported";
        } else if (!strcmp(hint, "switch-pro")) {
            stage = "special_protocol_disabled";
        } else if (!selected.diagnostic.config_valid) {
            stage = "malformed_configuration";
        } else if (selected.diagnostic.report_attempted && selected.diagnostic.report_errno) {
            stage = "hid_report_descriptor";
            errno = selected.diagnostic.report_errno;
        } else if (selected.diagnostic.report_attempted && !selected.diagnostic.report_valid) {
            stage = "hid_report_unverified";
        }
        goto reject;
    }
    memcpy(matched.node, selected.node, sizeof(matched.node));
    matched.diagnostic = selected.diagnostic;
    selected = matched;
    n->device = selected;
    n->iface_index = selected.iface_index;
    log_line("usb %s: %s candidate %04x:%04x interface=%u index=%u IN=%#x OUT=%#x", selected.node,
             selected.kind == UC_USB_XINPUT ? "XInput" : "Generic HID", selected.vid, selected.pid,
             selected.iface, selected.iface_index, selected.ep_in, selected.ep_out);
    stage = "driver_detach";
    /* Detach only this verified XInput or gamepad HID interface, never
     * the whole device. No-driver/unsupported detach may still permit a claim. */
    selected.diagnostic.detach_attempted = 1;
    if (ioctl(n->fd, USB_IFACE_DRIVER_DETACH, &n->iface_index) == 0) {
        n->detached = selected.diagnostic.detached = 1;
        log_line("usb %s: interface driver detached", selected.node);
    } else {
        selected.diagnostic.detach_errno = errno;
        log_line("usb %s: detach errno=%d; attempting claim", selected.node, errno);
    }
    stage = "claim_interface";
    selected.diagnostic.claim_attempted = 1;
    if (ioctl(n->fd, USB_CLAIM_INTERFACE, &n->iface_index) == 0) {
        n->claimed = selected.diagnostic.claimed = 1;
    } else {
        selected.diagnostic.claim_errno = errno;
        if (errno != ENOTTY && errno != ENOSYS && errno != EOPNOTSUPP)
            goto reject;
        log_line("usb %s: claim ioctl unsupported errno=%d; trying selected endpoints",
                 selected.node, errno);
    }
    stage = "input_endpoint_open";
    snprintf(path, sizeof(path), "/dev/usb/%u.%u.%u", bus, address, selected.ep_in & 15);
    n->in_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (n->in_fd < 0)
        goto reject;
    if (selected.kind == UC_USB_XINPUT) {
        stage = "output_endpoint_open";
        snprintf(path, sizeof(path), "/dev/usb/%u.%u.%u", bus, address, selected.ep_out & 15);
        n->out_fd = open(path, O_WRONLY | O_NONBLOCK);
        if (n->out_fd < 0)
            goto reject;
    }
    stage = "short_transfer";
    int one = 1;
    if (ioctl(n->in_fd, USB_SET_RX_SHORT_XFER, &one) < 0)
        goto reject;
    log_line("usb %s: input endpoint ready protocol=%s; output=%s", selected.node,
             selected.kind == UC_USB_XINPUT ? "XInput" : "Generic HID",
             n->out_fd >= 0 ? "available" : "disabled");
    *out = selected;
    return 1;
reject: {
    int saved_errno = errno;
    if (report) {
        snprintf(selected.reason, sizeof(selected.reason), "%s", stage);
        selected.error_number = saved_errno;
        *out = selected;
        /* One stage log per scan context. The API keeps the latest per-node result. */
        if (!(n->diagnostic_mask & 1u)) {
            n->diagnostic_mask |= 1u;
            log_line("usb %s: rejected stage=%s path=%s vid=%04x pid=%04x errno=%d (%s) "
                     "device_desc=%d/%d desc_errno=%d device_info=%d/%d info_errno=%d",
                     selected.node, stage, path, selected.vid, selected.pid, saved_errno,
                     saved_errno ? strerror(saved_errno) : "none",
                     selected.diagnostic.descriptor_attempted, selected.diagnostic.descriptor_ok,
                     selected.diagnostic.descriptor_errno, selected.diagnostic.info_attempted,
                     selected.diagnostic.info_ok, selected.diagnostic.info_errno);
        }
    }
    DIR *directory = n->directory;
    n->directory = NULL;
    native_close(n);
    n->directory = directory;
    return report ? 2 : 0;
}
}

static int native_scan(void *context, uc_usb_device *out)
{
    native_transport *n = context;
    if (n->fd >= 0 || !out)
        return -1;
    if (!n->directory)
        n->directory = opendir("/dev");
    if (!n->directory) {
        log_line("usb: opendir /dev failed errno=%d", errno);
        return -1;
    }
    for (unsigned examined = 0; examined < 128; examined++) {
        struct dirent *entry = readdir(n->directory);
        if (!entry) {
            closedir(n->directory);
            n->directory = NULL;
            return 0;
        }
        unsigned bus, address;
        char trailing;
        if (sscanf(entry->d_name, "ugen%u.%u%c", &bus, &address, &trailing) != 2 || bus > 255 ||
            address > 127 || !address)
            continue;
        return open_candidate(n, bus, address, out);
    }
    return 0;
}

static int native_read(void *context, uint8_t *buffer, size_t capacity)
{
    native_transport *n = context;
    if (n->in_fd < 0 || n->dead || !buffer || capacity < n->device.packet_in)
        return -1;
    ssize_t count = read(n->in_fd, buffer, n->device.packet_in);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    if (!count)
        return 0; /* Empty interrupt packet; health checks detect detach. */
    if (count < 0) {
        n->dead = 1;
        log_line("usb %s: input stopped bytes=%zd errno=%d", n->device.node, count,
                 count < 0 ? errno : 0);
        return -1;
    }
    if (n->reads++ == 0)
        log_line("usb %s: first packet bytes=%zd header=%02x %02x", n->device.node, count,
                 buffer[0], count > 1 ? buffer[1] : 0);
    return (int)count;
}

static int native_write(void *context, const uint8_t *buffer, size_t length)
{
    native_transport *n = context;
    if (n->out_fd < 0 || n->dead || n->output_dead || !buffer || !length ||
        length > n->device.packet_out)
        return -1;
    ssize_t count = write(n->out_fd, buffer, length);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    if (count != (ssize_t)length) {
        n->output_dead = 1;
        log_line("usb %s: output failed bytes=%zd expected=%zu errno=%d", n->device.node, count,
                 length, count < 0 ? errno : 0);
        return -1;
    }
    return (int)count;
}

static int native_alive(void *context)
{
    native_transport *n = context;
    if (n->fd < 0 || n->dead)
        return 0;
    struct usb_device_info info;
    memset(&info, 0, sizeof(info));
    if (ioctl(n->fd, USB_GET_DEVICEINFO, &info) < 0)
        return errno == ENXIO || errno == ENODEV ? 0 : -1;
    return info.udi_vendorNo == n->device.vid && info.udi_productNo == n->device.pid;
}
static const uc_usb_ops ops = {native_scan, native_read, native_write, native_alive, native_close};
int uc_usb_transport_open(uc_usb_transport *out)
{
    if (!out)
        return 0;
    int already_running = 0;
    for (unsigned i = 0; i < 4; i++)
        already_running |= natives[i].leased;
    if (!already_running) {
        hid_cache_count = hid_cache_next = hid_presence_next = 0;
        memset(hid_cache, 0, sizeof(hid_cache));
    }
    for (unsigned i = 0; i < 4; i++) {
        native_transport *n = &natives[i];
        if (n->leased)
            continue;
        memset(n, 0, sizeof(*n));
        n->fd = n->in_fd = n->out_fd = -1;
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
int uc_usb_transport_take_removed(char *node, size_t capacity)
{
    (void)node;
    (void)capacity;
    return 0;
}
#endif
