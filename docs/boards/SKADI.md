# Skadi

Skadi is the reference split board in this repository.

It is an nRF5340 split keyboard with separate `left` and `right` CPUAPP variants and the standard nRF5340 CPUNET image for Bluetooth.

## Board layout

The board lives in `boards/ykb/skadi`.

Since the board is practically identical for both left and right halves, most of the files are organized as "common" and just inlcuded in the final DTS files.

Important files:

- `board.yml` - declares `skadi/nrf5340/cpuapp/left` and `skadi/nrf5340/cpuapp/right`.
- `cpuapp/common.dts` - shared CPUAPP hardware: USB, ADC, I2C, LEDs, flash layout and power pins.
- `cpuapp/kb-handler.dts` - key scanning, muxes and `zephyr,user` keyboard properties.
- `cpuapp/backlight.dts` - WS2812/SK6812-style LED strip through I2S.
- `cpuapp/battsense.dts` - charger and fuel-gauge devices.
- `resources/default_settings.txt` - default thresholds, calibration, layers, FN shortcuts and mouse emulation keys.
- `resources/backlight_layout.txt` - LED/key coordinates used by Lumiscript.

## Split roles

The left half is the master. It enables the SplitLink master KBHandler implementation and owns host transports such as USB/Bluetooth HID.

The right half is the slave. It scans its local keys and sends values through SplitLink. It can still run local subsystems such as backlight, but host reports are produced by the master.

Skadi defines:

- `kb-handler-key-count = <30>`
- `kb-handler-key-count-slave = <30>`

So the total logical keyboard has 60 keys.

## KScan

Skadi uses `kscan-muxes`.

There are four GPIO muxes and four ADC channels:

- mux1 handles keys 0-7.
- mux2 handles keys 8-15.
- mux3 handles keys 16-23.
- mux4 handles keys 24-29.

Both halves use local indices starting at zero. The slave indices become global right-half indices when SplitLink/KBHandler moves them into the master key space.

## Backlight

Skadi has a 30 LED strip per half. The LED strip is configured as `worldsemi,ws2812-i2s`.

The brightness cap is intentionally low:

```dts
ykb-backlight-max-abs-brightness = <7>;
```

Do not raise this casually. These LEDs can pull a stupid amount of current at full white.

## Power and battery

Skadi uses:

- A GPIO power/recovery button on `gpio0 13`.
- A PWM power indicator LED.
- A shutdown GPIO on `gpio0 14`.
- BQ25185 charger state pins.
- MAX17048 fuel gauge over I2C.

YKB Power handles the power indicator and long-press shutdown. YKB BattSense handles charger/fuel-gauge state and can schedule shutdown on critical battery state.

## Build targets

```sh
west build -b skadi/nrf5340/cpuapp/left app -d build-left
west build -b skadi/nrf5340/cpuapp/right app -d build-right
```

or

```sh
west ykb-build skadi
```

The sysbuild setup also builds MCUboot, netboot and the CPUNET image.
