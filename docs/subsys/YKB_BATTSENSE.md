# YKB BattSense

YKB BattSense monitors battery state.

It reads a charger device and a fuel-gauge device, publishes state changes, and can request shutdown on critical battery state.

## Devicetree

BattSense is enabled when `/zephyr,user` contains both:

```dts
ykb-battsense-charger = <&charger>;
ykb-battsense-fuel-gauge = <&fuel_gauge>;
```

The charger must implement Zephyr charger API.

The fuel gauge must implement Zephyr fuel gauge API and support relative state of charge.

## Runtime state

State is:

```c
typedef struct {
    enum charger_status charge_status;
    uint8_t percentage;
} ykb_battsense_state_t;
```

Read it with:

```c
int ykb_battsense_get_state(ykb_battsense_state_t *out_state);
```

## Settings

BattSense settings live inside KBSettings:

- `low_threshold`
- `thread_sleep_ms`

Defaults come from Kconfig:

- `CONFIG_YKB_BATTSENSE_LOW_THRESHOLD`
- `CONFIG_YKB_BATTSENSE_THREAD_DEFAULT_SLEEP_TIME`

Critical threshold is currently Kconfig-only:

- `CONFIG_YKB_BATTSENSE_CRIT_THRESHOLD`

## Callbacks

Use `YKB_BATTSENSE_DEFINE` to listen for battery events.

```c
YKB_BATTSENSE_DEFINE(my_subsys) = {
    .on_state_changed = ...,
    .on_low_percentage = ...,
    .on_critical_percentage = ...,
};
```

Low and critical callbacks are only fired when percentage crosses downward.

## Shutdown behavior

If percentage falls below the critical threshold, BattSense calls critical callbacks and schedules shutdown through YKB Power.

If `CONFIG_YKB_BATTSENSE_SHUTOFF_ON_CHARGER_FAILURE` is enabled, charger health failure can also trigger shutdown.

Shutdown only works if YKB Power has a shutdown GPIO.

## Important Kconfig

- `CONFIG_YKB_BATTSENSE_THREAD_STACK_SIZE`
- `CONFIG_YKB_BATTSENSE_THREAD_PRIORITY`
- `CONFIG_YKB_BATTSENSE_THREAD_DEFAULT_SLEEP_TIME`
- `CONFIG_YKB_BATTSENSE_LOW_THRESHOLD`
- `CONFIG_YKB_BATTSENSE_CRIT_THRESHOLD`
