# Adding new board

The process of adding new boards should be as straightforward as writing Device Tree files, some resource files and some drivers if required.

## Adding new board to `boards` directory

- Create `boards/board_brand/board_name` directory.
- The best way to continue with Device Tree files is to find a similar board in Zephyr's boards or nRF SDK boards directories and shaping those files to your needs.
- Add [resources](#resources) inside your board directory.

### Adding split keyboard

If adding split keyboard, the board in the directory should declare two variants of the SOC used: `left` and `right`. This is done in `boards/board_brand/board_name/board.yml`.

For example:

```yaml
board:
  name: someboard
  full_name: Some Board
  vendor: some
  socs:
  - name: 'nrf52840'
    variants:
    - name: 'left'
    - name: 'right'
```

or for multi-core SOCs:

```yaml
board:
  name: skadi 
  full_name: YKB Skadi
  vendor: ykb
  socs:
  - name: 'nrf5340'
    variants:
    - name: 'left'
      cpucluster: 'cpuapp'
    - name: 'right'
      cpucluster: 'cpuapp'
```

This will require having separate DTS files and defconfigs for those variants even if the boards are identical. But you can abstract the things you need in common DTS files and include them from both DTS files. For defconfigs you can have one and symlink it to left and right variants.

## Resources

To extend board configuration and not bloat DTS this project uses resources to generate some configurations.

The resources use `ini`-like style. The array members are separated by any amount of spaces to make them more readable.

**If the resource file is for a split keyboard** then `|` symbol can be used to separate array values for left and right variants. Values to the left of `|` will be used for the left board while values to the right are for the right variant. You can separate the values however you want to organize the values.

For example:

```
[some_array]
0  1  2  3  4  5  | 0  1  2  3  4  5
6  7  8  9  10 11 | 6  7  8  9  10 11
12 13 14 15 16 17 | 12 13 14 15 16 17
18 19 20 21 22 23 | 18 19 20 21 22 23
24 25 26 27 28 29 | 24 25 26 27 28 29
```

```
[some_array]
0  1  2  3  4  5  | 0  1  2  3  4  5
6  7  8  9  10 11 | 6  7  8  9  10 11
12 13 14 15 16 17 | 12 13 14 15 16 17
18 19 20 21 22 23 | 18 19 20 21 22 23
24 25 26 | 24 25 26
27 28 29 | 27 28 29
```

```
[some_array]
0  1  2  3  4  5  |
6  7  8  9  10 11 |
12 13 14 15 16 17 |
18 19 20 21 22 23 |
24 25 26 27 28 29 |
| 0  1  2  3  4  5
| 6  7  8  9  10 11
| 12 13 14 15 16 17
| 18 19 20 21 22 23
| 24 25 26 27 28 29
```

All of the above examples are appropriate.

### Backlight layout

`boards/board_brand/board_name/resources/backlight_layout.txt` contains information for the YKB Backlight subsystem so that it can understand how to use the RGB LED strip for backlighting. Should be omitted if the device doesn't have RGB strip backlighting.

The file should contain the following categories: `led_map`, `x_coordinates`, `y_coordinates`.

`led_map` is the array of indices to map the index of the key reported from KScan driver into the index of LED on the strip.

`x_coordinates` and `y_coordinates` are arrays of values in the range of 0-1000 of relative coordinates of the switches or LEDs. The (0,0) coordinate should be the top left LED of the keyboard. So `y_coordinates` increase toward the bottom LEDs and `x_coordinates` increase toward right LEDs. It is recommended to scale the coordinates to take full range of values up to a 1000 on at least one of the axes.

For example:

```
[led_map]
0  1  2  3  4  5
6  7  8  9  10 11
12 13 14 15 16 17
18 19 20 21 22 23
24 25 26 27 28 29

[x_coordinates]
0   0   0   0   149 149
149 149 297 297 297 297
419 419 419 419 540 540
540 540 662 662 662 662
695 797 884 945 999 884

[y_coordinates]
0   115 223 338 27  142
250 365 0   115 223 338
14  128 236 351 0   115
223 338 0   115 223 338
608 662 763 655 493 459
```

or for the split keyboards:

```
[led_map]
0  1  2  3  4  5  | 0  1  2  3  4  5
6  7  8  9  10 11 | 6  7  8  9  10 11
12 13 14 15 16 17 | 12 13 14 15 16 17
18 19 20 21 22 23 | 18 19 20 21 22 23
24 25 26 27 28 29 | 24 25 26 27 28 29

[x_coordinates]
0   0   0   0   149 149 | 337 337 337 337 486 486
149 149 297 297 297 297 | 486 486 634 634 634 634
419 419 419 419 540 540 | 756 756 756 756 877 877
540 540 662 662 662 662 | 877 877 999 999 999 999
695 797 884 945 999 884 | 304 202 115 54  0   115

[y_coordinates]
0   115 223 338 27  142 | 0   115 223 338 27  142
250 365 0   115 223 338 | 250 365 0   115 223 338
14  128 236 351 0   115 | 14  128 236 351 0   115
223 338 0   115 223 338 | 223 338 0   115 223 338
608 662 763 655 493 459 | 608 662 763 655 493 459
```

### Default settings

`boards/board_brand/board_name/resources/default_settings.txt` contains default runtime settings for KBSettings. It is compiled into firmware and used when persistent settings are not present or cannot be loaded.

This file should match the keyboard key count exactly. For a split keyboard it should contain both halves, with left-side values before `|` and right-side values after `|`.

The important sections are:

- `thresholds` - raw ADC value where a key becomes pressed.
- `minimums` - raw ADC value treated as 0% pressed.
- `maximums` - raw ADC value treated as 100% pressed.
- `deadzones` - percent deadzone used when converting raw ADC values into press percentage.
- `layer1`, `layer2`, `layer3` - HID mappings for each physical key.
- `fn_shortcuts` - actions triggered by holding FN and pressing another key.
- `mouseemu` - mouse emulation runtime settings.
- `move_keys`, `scroll_keys`, `button_keys` - physical key indices used by mouse emulation.

Layer values are symbolic names from `include/dt-bindings/kb-handler/kb-key-codes.h` just without the `KEY_` prefix. It is highly recommended to use the same modifier keys and layer keys on all layers to avoid any issues.

Example:

```
[thresholds]
600 600 600 | 600 600 600

[minimums]
500 500 500 | 500 500 500

[maximums]
900 900 900 | 900 900 900

[deadzones]
20 20 20 | 20 20 20

[layer1]
Q W E | I O P

[layer2]
F1 F2 F3 | F8 F9 F10

[layer3]
NOKEY NOKEY NOKEY | NOKEY NOKEY NOKEY

[fn_shortcuts]
FN+Q = BL_TOGGLE
FN+W = MODE_MOUSESIM
FN+E = MODE_NORMAL
```

## Device Tree conventions

Most YKB subsystems are discovered through properties under `/zephyr,user`.

Common properties:

- `kb-handler-kscans` - phandles to KScan devices used by KBHandler.
- `kb-handler-key-count` - local key count.
- `kb-handler-key-count-slave` - slave key count for split keyboards.
- `ykb-backlight` - LED strip device for YKB Backlight.
- `ykb-backlight-max-abs-brightness` - hard cap for LED brightness in percent.
- `ykb-battsense-charger` - charger device for BattSense.
- `ykb-battsense-fuel-gauge` - fuel-gauge device for BattSense.
- `ykb-power-button` - GPIO key used as power button.
- `ykb-power-indicator` - LED device used as power indicator.
- `ykb-power-shutdown-gpios` - GPIO that triggers board shutdown.

## Build checks

After adding a board, build every variant:

```sh
west ykb-build board_name
```

For normal keyboards this will build in `build` directory, and for split keyboards this will build in `build-left` and `build-right` respectively.
