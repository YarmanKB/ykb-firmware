# KBSettings

KBSettings owns runtime keyboard settings.

It loads defaults, restores saved settings from Zephyr settings storage, applies updates, saves them, and notifies interested subsystems when settings change.

## What is stored

The main settings object is `kb_settings_t`.

It contains:

- Active mode: normal, race or mouse emulation.
- Per-key thresholds, minimums, maximums and deadzones.
- Three mapping layers.
- Mouse emulation settings.
- Transport priority.
- FN shortcuts.
- Optional BattSense settings.
- Optional Backlight settings.
- Optional Power settings.

The key arrays use `TOTAL_KEY_COUNT`, which is local key count plus slave key count when the board is split.

## Defaults

Defaults come from generated resources, usually from:

```text
boards/<vendor>/<board>/resources/default_settings.txt
```

The resource file is intentionally human-readable. It should be easier to tune a keyboard by editing calibration values than by changing C initializers.

For split boards, values before `|` are left/master values and values after `|` are right/slave values.

## API

```c
int kb_settings_get(kb_settings_t *settings);
int kb_settings_apply(const kb_settings_t *settings);
```

`kb_settings_get()` copies the current settings.

`kb_settings_apply()` applies new settings, saves them to persistent storage, and triggers update callbacks.

Both functions can block. Do not call them from timing-sensitive scan loops.

## Update callbacks

Use `ON_SETTINGS_UPDATE_DEFINE` to subscribe to settings changes.

```c
static void on_settings_update(const kb_settings_t *settings) {}

ON_SETTINGS_UPDATE_DEFINE(my_subsys, on_settings_update);
```

This is how KBHandler, Backlight, BattSense, Power and SplitLink keep their cached settings in sync.

## Devicetree inputs

KBSettings gets key counts from `/zephyr,user`:

- `kb-handler-key-count`
- `kb-handler-key-count-slave`

The generated default settings must match those counts. If the arrays are short or long, fix the resource file.

## Gotchas

Saved settings override generated defaults. If a firmware change appears to do nothing, clear settings storage or explicitly apply new settings through VendorHID.

Do not add subsystem-specific global variables when the value is runtime user configuration. Put it in `kb_settings_t` so it can be saved, synchronized and exposed to the configurator.
