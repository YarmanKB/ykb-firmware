# YKB Power

YKB Power manages simple board power behavior.

Right now it handles:

- Power indicator LED.
- Power button polling.
- Shutdown GPIO triggering.

It is not a full low-power policy manager yet, but should and will be.

## Devicetree

YKB Power looks at `/zephyr,user`.

Optional properties:

```dts
ykb-power-button = <&button0>;
ykb-power-indicator = <&led1>;
ykb-power-shutdown-gpios = <&gpio0 14 GPIO_ACTIVE_HIGH>;
```

The indicator can be a GPIO LED or PWM LED. Breathe mode only exists when brightness control is supported.

## Indicator modes

Supported modes:

- `OFF`
- `STATIC`
- `BLINKING`
- `BREATHE` when brightness control exists.

Settings live in `kb_settings_t.power`.

For blinking, `speed` is the on/off interval.

For breathe, `speed` is one full breathe cycle.

## Power button shutdown

If both power button and shutdown GPIO exist, YKB Power polls the button.

When the button has been held for `shutdown_time` milliseconds, the shutdown GPIO is set active.

Default `shutdown_time` is 1000 ms.

## Public API

```c
int ykb_power_schedule_shutdown(k_timeout_t timeout);
const ykb_power_settings_t *ykb_power_get_default_settings(void);
```

`ykb_power_schedule_shutdown()` returns `-ENODEV` if the board has no shutdown GPIO.

BattSense uses this API for critical battery shutdown.

## Important Kconfig

- `CONFIG_YKB_POWER_THREAD_SLEEP_TIME` - button/breathe polling interval.
- `CONFIG_YKB_POWER_THREAD_STACK_SIZE`
- `CONFIG_YKB_POWER_THREAD_PRIORITY`

The generated presence symbols are based on devicetree:

- `CONFIG_YKB_POWER_INDICATOR_PRESENT`
- `CONFIG_YKB_POWER_BUTTON_PRESENT`
- `CONFIG_YKB_POWER_SHUTDOWN_GPIOS_PRESENT`
