#include "hid_devices.h"

#include <subsys/kb_handler.h>
#include <subsys/usb_connect.h>
#include <subsys/vendor_hid_protocol.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(usb_connect, CONFIG_USB_CONNECT_LOG_LEVEL);

#define CUSTOM_IN_REPORT_SIZE (CONFIG_USB_CONNECT_MAX_VENDOR_IN_REPORT_SIZE)
#define CUSTOM_OUT_REPORT_SIZE (CONFIG_USB_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE)
#define CUSTOM_FEATURE_SIZE (CONFIG_USB_CONNECT_MAX_VENDOR_IN_REPORT_SIZE)

#define YKB_PROTOCOL_DATA_LENGTH                                               \
    (MIN(CUSTOM_IN_REPORT_SIZE, CUSTOM_OUT_REPORT_SIZE) - 6)

static const uint8_t hid_vendor_report_desc[] = {
    HID_USAGE_PAGE(0xFF),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),

    HID_USAGE(0x02),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX8(0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(CUSTOM_IN_REPORT_SIZE),
    HID_INPUT(0x02),

    HID_USAGE(0x03),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX8(0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(CUSTOM_OUT_REPORT_SIZE),
    HID_OUTPUT(0x02),

    HID_USAGE(0x04),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX8(0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(CUSTOM_FEATURE_SIZE),
    HID_FEATURE(0x02),

    HID_END_COLLECTION,
};

static const struct device *hid_vendor_dev =
    DEVICE_DT_GET(DT_NODELABEL(hid_vendor));
static vendor_hid_protocol_ctx_t vendor_protocol_ctx;

static atomic_bool __ready;
static uint32_t __duration;
static atomic_bool boot_mode;

static int usb_vendor_send_packet(const uint8_t *data, size_t len,
                                  void *user_data) {
    uint8_t report[CONFIG_USB_CONNECT_MAX_VENDOR_IN_REPORT_SIZE] = {0};
    if (len > sizeof(report)) {
        return -EMSGSIZE;
    }

    memcpy(report, data, len);

    int err = hid_device_submit_report(hid_vendor_dev, sizeof(report), report);
    if (err) {
        LOG_ERR("hid_device_submit_report (vendor): %d", err);
    }

    return err;
}

static void iface_ready(const struct device *dev, const bool ready) {
    LOG_INF("HID device %s interface is %s", dev->name,
            ready ? "ready" : "not ready");
    ATOMIC_STORE(&__ready, ready);
}

static int get_report(const struct device *dev, const uint8_t type,
                      const uint8_t id, const uint16_t len,
                      uint8_t *const buf) {
    if (len < 1 || len > CONFIG_USB_CONNECT_MAX_VENDOR_IN_REPORT_SIZE) {
        LOG_WRN("Unsupported report length %d", len);
        return -ENOTSUP;
    }

    return 0;
}

static int set_report(const struct device *dev, const uint8_t type,
                      const uint8_t id, const uint16_t len,
                      const uint8_t *const buf) {
    if (type != HID_REPORT_TYPE_OUTPUT) {
        LOG_WRN("Unsupported report type");
        return -ENOTSUP;
    }

    if (len < 1 || len > CONFIG_USB_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE) {
        LOG_WRN("Unsupported report length %d", len);
        return -ENOTSUP;
    }

    return vendor_hid_protocol_parse(&vendor_protocol_ctx, buf, len);
}

static void set_idle(const struct device *dev, const uint8_t id,
                     const uint32_t duration) {
    LOG_INF("Set Idle %u to %u", id, duration);
    __duration = duration;
}

static uint32_t get_idle(const struct device *dev, const uint8_t id) {
    LOG_INF("Get Idle %u to %u", id, __duration);
    return __duration;
}

static void output_report(const struct device *dev, const uint16_t len,
                          const uint8_t *const buf) {
    LOG_HEXDUMP_DBG(buf, len, "o.r.");
    set_report(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}

static void set_protocol(const struct device *dev, const uint8_t proto) {
    ATOMIC_STORE(&boot_mode, proto == HID_PROTOCOL_BOOT);
    LOG_INF("hid_vendor in %s mode",
            proto == HID_PROTOCOL_BOOT ? "boot" : "report");
}

static struct hid_device_ops ops = {
    .set_protocol = set_protocol,
    .iface_ready = iface_ready,
    .get_report = get_report,
    .set_report = set_report,
    .set_idle = set_idle,
    .get_idle = get_idle,
    .output_report = output_report,
};

static int usb_connect_init_vendor_hid(void) {
    int err;

    if (!device_is_ready(hid_vendor_dev)) {
        LOG_ERR("hid_vendor not ready");
        return -EIO;
    }

    err = vendor_hid_protocol_init(&vendor_protocol_ctx, usb_vendor_send_packet,
                                   NULL);
    if (err) {
        LOG_ERR("vendor_hid_protocol_init: %d", err);
        return err;
    }

    err = hid_device_register(hid_vendor_dev, hid_vendor_report_desc,
                              sizeof(hid_vendor_report_desc), &ops);
    if (err) {
        LOG_ERR("hid_vendor register: %d", err);
        return err;
    }

    return 0;
}

USB_CONNECT_REGISTER_HID_DEVICE(usb_vendor, usb_connect_init_vendor_hid);
