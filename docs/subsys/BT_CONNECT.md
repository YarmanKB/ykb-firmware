# BTConnect

BTConnect exposes the keyboard as Bluetooth HID.

It owns Bluetooth initialization, HID service setup, optional battery service and optional VendorHID over Bluetooth.

## Features

BTConnect can provide:

- Keyboard HID reports.
- Mouse HID reports.
- Vendor HID reports for YKB Configurator.
- Battery Service for the local half.
- Optional secondary Battery Service for the split slave.

The feature set is controlled by Kconfig.

## Public API

```c
bool bt_connect_can_send_kb_report(void);
void bt_connect_send_kb_report(const hid_kb_report_t *report);

bool bt_connect_can_send_mouse_report(void);
void bt_connect_send_mouse_report(const hid_mouse_report_t *report);

void bt_connect_set_battery_level(uint8_t percentage);
```

KBHandler uses the keyboard/mouse functions through its transport selection layer.

BattSense updates battery level through the battery integration.

Connection callbacks:

```c
BT_CONNECT_CB_DEFINE(name) = {
    .on_connect = ...,
    .on_disconnect = ...,
};
```

## HID report map

While with USB HID we can declare multiple HID devices and each of those can have its own report map, it is not practical with Bluetooth HID. So we need to have one report.

Bluetooth HID devices register report fragments through iterable sections. BTConnect assembles them into one HID service report map.

This is why keyboard, mouse and vendor HID support can be enabled and compiled independently.

## Pairing and security

BTConnect selects Zephyr Bluetooth settings and SMP. HID permissions default to encrypted read/write.

Board defaults usually set:

- `BT_DEVICE_NAME`
- DIS manufacturer/model/PNP values.
- `BT_MAX_CONN`
- `BT_MAX_PAIRED`

For split boards using Bluetooth SplitLink and Bluetooth HID at the same time, connection counts need to account for both host and split connections.

## Vendor HID over Bluetooth

When `BT_CONNECT_VENDOR` is enabled, BTConnect uses the same `VendorHID` protocol implementation as USBConnect.

The packet sizes are limited to:

- `CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE`
- `CONFIG_BT_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE`

## Important Kconfig

- `CONFIG_BT_CONNECT_KBD` - Enable keyboard HID.
- `CONFIG_BT_CONNECT_MOUSE` - Enable mouse HID.
- `CONFIG_BT_CONNECT_VENDOR` - Enable vendor HID for YKB Configurator.
- `CONFIG_BT_CONNECT_BAS` - Enable Battery Service for local battery.
- `CONFIG_BT_CONNECT_SPLIT_BAS` - [EXPERIMENTAL] Enable Battery Service for secondary split slave battery if present. Available only for SplitLink master. Can make battery recognition on the host incorrect.
- `CONFIG_BT_CONNECT_REPORT_MAP_MAX_SIZE` - Maximum size of HID report.
