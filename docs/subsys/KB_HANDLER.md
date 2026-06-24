# KBHandler

KBHandler is the central keyboard behavior subsystem.

It consumes raw KScan values, applies thresholds/calibration from KBSettings, builds HID keyboard and mouse reports, and notifies other subsystems about key transitions and press percentages.

## Implementations

`CONFIG_KB_HANDLER` selects one implementation:

- `KB_HANDLER_SIMPLE` - normal single-board keyboard.
- `KB_HANDLER_SPLITLINK_MASTER` - split master. Receives slave values and sends host reports.
- `KB_HANDLER_SPLITLINK_SLAVE` - split slave. Scans local keys and forwards them through SplitLink.

The implementation is usually selected by board defconfig.

## Input path

KBHandler owns the scan loop. Its thread calls `kscan_scan()` for each configured KScan device, then processes the completed local scan frame.

The message queue is used for settings sync and split slave updates, not for every raw local ADC sample.

Raw values are interpreted using:

- `thresholds` - pressed/released decision.
- `minimums` - value treated as 0% pressed.
- `maximums` - value treated as 100% pressed.
- `deadzones` - ignored low-end percentage range.

`on_new_value` callbacks receive a percentage from `0` to `100`, not the raw ADC value.

## Reports

Keyboard reports are standard 6KRO HID reports with modifier byte.

If more than six non-modifier keys are pressed and `CONFIG_KB_HANDLER_REPORT_ROLLOVER` is enabled, KBHandler sends rollover error key codes.

Mouse reports are used by mouse emulation mode. Mouse emulation keys are removed from the keyboard report while `KB_MODE_MOUSESIM` is active.

Transport selection is handled by `kbh_prio` in settings:

- USB first
- Bluetooth first

If only one transport is compiled in, there is nothing meaningful to choose.

## Layers and FN

The settings contain three layers:

- `mappings_layer1`
- `mappings_layer2`
- `mappings_layer3`

Layer keys and FN keys are symbolic HID values from `kb-key-codes.h`.

FN shortcuts are configured in settings and can change mode, transport, backlight script, brightness and speed.

## Public callbacks

Use `KB_HANDLER_CB_DEFINE` when another subsystem needs keyboard events.

```c
static void on_event(uint16_t index, bool pressed) {}
static void on_value(uint16_t index, uint16_t percent) {}

KB_HANDLER_CB_DEFINE(my_subsys) = {
    .on_event = on_event,
    .on_new_value = on_value,
};
```

These callbacks are the right API for backlight and other behavior that needs processed key transitions. Do not subscribe directly to KScan unless raw scan values are really what you need.

## Split behavior

On split master, slave values are received as the second half of the global key array.

On split slave, local scans are sent through SplitLink. Local subsystems that use `KB_HANDLER_CB_DEFINE` may see global right-half indices, so they should map back to local layout indices if they own local arrays.

## Important Kconfig

- `CONFIG_KB_HANDLER_MSGQ_SIZE` - queue depth for settings and split events.
- `CONFIG_KB_HANDLER_THREAD_STACK_SIZE` - stack for report building and settings sync.
- `CONFIG_KB_HANDLER_THREAD_PRIORITY` - should be lower priority number than slow background subsystems.
- `CONFIG_KB_HANDLER_REPORT_ROLLOVER` - whether to emit rollover error reports.
