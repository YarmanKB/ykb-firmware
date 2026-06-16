# YKB Backlight

YKB Backlight drives LED strip backlighting using LumiscriptVM.

It owns default script slots, persistent script storage, layout coordinates and the render thread.

## Hardware

The subsystem is enabled when `/zephyr,user` has:

```dts
ykb-backlight = <&led_strip>;
```

The referenced device must be a Zephyr LED strip device.

Optional:

```dts
ykb-backlight-max-abs-brightness = <7>;
```

This is a hard board-level power cap. Runtime brightness is multiplied by this cap.

## Layout resource

Backlight layout comes from:

```text
boards/<vendor>/<board>/resources/backlight_layout.txt
```

Required sections:

- `led_map`
- `x_coordinates`
- `y_coordinates`

Coordinates are `0..1000` relative values. Lumiscript receives those values per key.

For split boards, the generated layout contains both halves. At runtime each half uses its local slice.

## Lumiscript resources

Default scripts live in:

```text
resources/lumiscript/*.lumi
```

During the Zephyr build, the resource generator builds the host `lumic` compiler from the lumiscript submodule, compiles `.lumi` scripts into `.lbc`, and emits the bytecode into generated C resources.

Host build requirements:

- `meson`
- `ninja`
- a native C compiler

The firmware stores script slots as bytecode. The source scripts are not included in firmware.

## Runtime settings

Backlight settings live in `kb_settings_t.backlight`:

- `on`
- `active_script_index`
- `speed`
- `brightness`
- `thread_sleep_ms`

Settings updates reload the active script and reset the VM state.

## Persistent script slots

Script slots are saved under the `ykb_bl` settings subtree.

Public API:

```c
size_t ykb_backlight_get_script_slot_count(void);
int ykb_backlight_get_script_slot(uint16_t slot, ykb_backlight_script_slot_t *out);
int ykb_backlight_set_script_slot(uint16_t slot, const ykb_backlight_script_slot_t *in);
int ykb_backlight_get_script_slot_crc32(uint16_t slot, uint32_t *out_crc32);
```

VendorHID uses this API for configurator upload/download.

SplitLink uses CRCs to sync script slots from master to slave.

## Important Kconfig

- `CONFIG_YKB_BL_THREAD_STACK_SIZE` - VM/render thread stack.
- `CONFIG_YKB_BL_SCRIPT_SLOT_COUNT` - persistent slot count.
- `CONFIG_YKB_BL_SCRIPT_SLOT_SIZE` - max uploaded bytecode size per slot.
- `CONFIG_YKB_BL_LUMIVM_*` - VM storage limits.
