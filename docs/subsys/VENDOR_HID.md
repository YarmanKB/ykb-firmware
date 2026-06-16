# VendorHID

VendorHID is the protocol used by YKB Configurator.

It is transport-independent. USBConnect and BTConnect provide packets; VendorHID parses requests, performs the operation, and sends response packets through a callback.

## Transport framing

VendorHID uses `lib/ykb_protocol` to split larger payloads into HID-sized packets.

That matters because settings and Lumiscript slots are larger than one 64-byte HID report.

## Requests

Current request types:

- `GET_FEATURES`
- `GET_VALUES`
- `GET_SETTINGS`
- `SET_SETTINGS`
- `GET_LUMISCRIPT_SLOT`
- `SET_LUMISCRIPT_SLOT`
- `GET_LUMISCRIPT_SLOT_INFO`
- `CLEAR_LUMISCRIPT_SLOT`
- `RENAME_LUMISCRIPT_SLOT`
- `GET_BATTERY_STATE`
- `GET_SECONDARY_BATTERY_STATE`

Some requests only exist when the related subsystem is compiled in.

## Payloads

Large payloads:

- `kb_settings_t` for settings.
- `vendor_hid_proto_script_slot_packet_t` for Lumiscript slots.

The maximum VendorHID payload size is selected at compile time as the larger of those.

## Response work

Requests are parsed from transport callbacks, but expensive responses and mutations are handled by a work item.

Keep it this way. Settings saves, script slot writes and flash operations should not run on tiny HID callback stacks.

## Split behavior

On a split master, VendorHID can expose secondary battery state if SplitLink sync is enabled.

Backlight script uploads happen on the master first. SplitLink then synchronizes script slots to the slave using script manifests and CRCs.

## Adding a request

When adding a request:

- Add request and response IDs in `include/subsys/vendor_hid_protocol.h`.
- Keep payload structs packed.
- Make the request conditional on the subsystem that owns the data.
- Parse only enough in the callback to select work.
- Send errors as `RESPONSE_ERROR`.

Do not put transport-specific behavior in `subsys/vendor_hid`. USB and Bluetooth should stay thin wrappers.
