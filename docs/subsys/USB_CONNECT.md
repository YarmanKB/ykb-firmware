# USBConnect

USBConnect exposes the keyboard as USB HID devices.

It owns USB device initialization and optional HID interfaces for keyboard, mouse and VendorHID.

## Hardware requirements

The board must have an enabled `zephyr,hid-device` under the USB device controller.

Common node labels:

- `hid_kbd`
- `hid_mouse`
- `hid_vendor`

The subsystem is enabled only when Zephyr sees HID device nodes.

## HID devices

USBConnect can compile three HID devices:

- `USB_CONNECT_KBD` - keyboard report device.
- `USB_CONNECT_MOUSE` - mouse report device.
- `USB_CONNECT_VENDOR` - vendor HID transport for YKB Configurator.

Each device is registered by a small module under `subsys/usb_connect/src/hid_devices`.

## Public API

```c
bool usb_connect_can_send_kb_report(void);
void usb_connect_send_kb_report(const hid_kb_report_t *report);

bool usb_connect_can_send_mouse_report(void);
void usb_connect_send_mouse_report(const hid_mouse_report_t *report);

void usb_connect_handle_wakeup(void);
```

KBHandler uses these functions through the transport selection layer.

Other code can subscribe to USB connect/disconnect events:

```c
USB_CONNECT_CB_DEFINE(name) = {
    .on_connect = ...,
    .on_disconnect = ...,
};
```

## Vendor HID

When `USB_CONNECT_VENDOR` is enabled, USBConnect passes output reports to `vendor_hid_protocol_parse()` and sends response packets back as vendor input reports.

The vendor report size comes from the HID devicetree node and is exposed through:

- `CONFIG_USB_CONNECT_MAX_VENDOR_IN_REPORT_SIZE`
- `CONFIG_USB_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE`

## Descriptors

USB VID/PID/product/manufacturer are Kconfig options:

- `CONFIG_USB_CONNECT_VID`
- `CONFIG_USB_CONNECT_PID`
- `CONFIG_USB_CONNECT_MANUFACTURER`
- `CONFIG_USB_CONNECT_PRODUCT`

Boards should set product-specific defaults in board Kconfig.

## Gotchas

USB report send functions can fail if the interface is not ready. KBHandler checks readiness before choosing the transport.

Vendor HID request handling can involve settings and flash writes. Keep large work out of USB callback stacks.
