#include "usbd_init.h"

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_connect_usbd_init);

/* By default, do not register the USB DFU class DFU mode instance. */
static const char *const blocklist[] = {
    "dfu_dfu",
    NULL,
};

USBD_DEVICE_DEFINE(usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   CONFIG_USB_CONNECT_VID, CONFIG_USB_CONNECT_PID);

USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfr, CONFIG_USB_CONNECT_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(product, CONFIG_USB_CONNECT_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(sn)));

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

static const uint8_t attributes =
    (IS_ENABLED(CONFIG_USB_CONNECT_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0) |
    (IS_ENABLED(CONFIG_USB_CONNECT_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0);

/* Full speed configuration */
USBD_CONFIGURATION_DEFINE(fs_config, attributes, CONFIG_USB_CONNECT_MAX_POWER,
                          &fs_cfg_desc);

/* High speed configuration */
USBD_CONFIGURATION_DEFINE(hs_config, attributes, CONFIG_USB_CONNECT_MAX_POWER,
                          &hs_cfg_desc);

#if CONFIG_USB_CONNECT_20_EXTENSION_DESC

static const struct usb_bos_capability_lpm bos_cap_lpm = {
    .bLength = sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes = 0UL,
};

USBD_DESC_BOS_DEFINE(usbext, sizeof(bos_cap_lpm), &bos_cap_lpm);

#endif // CONFIG_USB_CONNECT_20_EXTENSION_DESC

static void fix_code_triple(struct usbd_context *uds_ctx,
                            const enum usbd_speed speed) {
    if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS) ||
        IS_ENABLED(CONFIG_USBD_CDC_ECM_CLASS) ||
        IS_ENABLED(CONFIG_USBD_CDC_NCM_CLASS) ||
        IS_ENABLED(CONFIG_USBD_MIDI2_CLASS) ||
        IS_ENABLED(CONFIG_USBD_AUDIO2_CLASS) ||
        IS_ENABLED(CONFIG_USBD_VIDEO_CLASS)) {
        usbd_device_set_code_triple(uds_ctx, speed, USB_BCC_MISCELLANEOUS, 0x02,
                                    0x01);
    } else {
        usbd_device_set_code_triple(uds_ctx, speed, 0, 0, 0);
    }
}

struct usbd_context *usbd_setup_device(usbd_msg_cb_t msg_cb) {
    int err;

    err = usbd_add_descriptor(&usbd, &lang);
    if (err) {
        LOG_ERR("Failed to initialize language descriptor (%d)", err);
        return NULL;
    }

    err = usbd_add_descriptor(&usbd, &mfr);
    if (err) {
        LOG_ERR("Failed to initialize manufacturer descriptor (%d)", err);
        return NULL;
    }

    err = usbd_add_descriptor(&usbd, &product);
    if (err) {
        LOG_ERR("Failed to initialize product descriptor (%d)", err);
        return NULL;
    }

    IF_ENABLED(CONFIG_HWINFO, (err = usbd_add_descriptor(&usbd, &sn);))
    if (err) {
        LOG_ERR("Failed to initialize SN descriptor (%d)", err);
        return NULL;
    }

    if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&usbd) == USBD_SPEED_HS) {
        err = usbd_add_configuration(&usbd, USBD_SPEED_HS, &hs_config);
        if (err) {
            LOG_ERR("Failed to add High-Speed configuration");
            return NULL;
        }

        err = usbd_register_all_classes(&usbd, USBD_SPEED_HS, 1, blocklist);
        if (err) {
            LOG_ERR("Failed to add register classes");
            return NULL;
        }

        fix_code_triple(&usbd, USBD_SPEED_HS);
    }

    err = usbd_add_configuration(&usbd, USBD_SPEED_FS, &fs_config);
    if (err) {
        LOG_ERR("Failed to add Full-Speed configuration");
        return NULL;
    }

    err = usbd_register_all_classes(&usbd, USBD_SPEED_FS, 1, blocklist);
    if (err) {
        LOG_ERR("Failed to add register classes");
        return NULL;
    }

    fix_code_triple(&usbd, USBD_SPEED_FS);
    usbd_self_powered(&usbd, attributes & USB_SCD_SELF_POWERED);

    if (msg_cb != NULL) {
        err = usbd_msg_register_cb(&usbd, msg_cb);
        if (err) {
            LOG_ERR("Failed to register message callback");
            return NULL;
        }
    }

#if CONFIG_USB_CONNECT_20_EXTENSION_DESC

    usbd_device_set_bcd_usb(&usbd, USBD_SPEED_FS, 0x0201);
    usbd_device_set_bcd_usb(&usbd, USBD_SPEED_HS, 0x0201);

    err = usbd_add_descriptor(&usbd, &usbext);
    if (err) {
        LOG_ERR("Failed to add USB 2.0 Extension Descriptor");
        return NULL;
    }

#endif // CONFIG_USB_CONNECT_20_EXTENSION_DESC

    return &usbd;
}

struct usbd_context *usbd_init_device(usbd_msg_cb_t msg_cb) {
    int err;

    if (usbd_setup_device(msg_cb) == NULL) {
        return NULL;
    }

    err = usbd_init(&usbd);
    if (err) {
        LOG_ERR("Failed to initialize device support");
        return NULL;
    }

    return &usbd;
}
