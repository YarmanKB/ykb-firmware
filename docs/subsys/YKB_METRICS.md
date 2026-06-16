# YKB Metrics

YKB Metrics is a lightweight runtime instrumentation subsystem.

It counts scan rates, scan timing, ADC clipping, keyboard message queue pressure, key transitions and HID report sends.

## Enabling

```conf
CONFIG_YKB_METRICS=y
```

Periodic logging is controlled by:

```conf
CONFIG_YKB_METRICS_LOG=y
CONFIG_YKB_METRICS_LOG_INTERVAL_MS=1000
```

When disabled, metrics macros compile down to nothing useful and should not affect behavior.

## KScan metrics

KScan drivers report:

- Raw samples per second.
- Full scans per second.
- Average scan time.
- Maximum scan time.
- Read errors.

Example:

```text
kscan[kscan1:0] samples=1913/s scans=239/s avg=4170us max=31921us errors=0/s
```

`kscan1:0` means device `kscan1`, unit `0`. For `kscan-muxes`, units are muxes.

## ADC metrics

ADC key metrics track:

- Clip count.
- Widest observed min/max span.

Example:

```text
adc keys: clips=0 widest_span=key16:356
```

This is useful when tuning gain/reference and per-key calibration.

## KBHandler metrics

KBHandler reports message queue activity:

```text
kb msgq: key=0/s value=8402/s slave=0/s drops=0/0/0 max_depth=60 reports=81/1
```

The fields show event rates, drops, maximum queue depth and sent keyboard/mouse report counts.

If `max_depth` reaches `CONFIG_KB_HANDLER_MSGQ_SIZE`, the queue is saturated.

## Instrumentation API

Use macros from `include/subsys/ykb_metrics.h`:

- `YKB_METRICS_KSCAN_SAMPLE`
- `YKB_METRICS_KSCAN_READ_ERROR`
- `YKB_METRICS_KSCAN_SCAN_DONE`
- `YKB_METRICS_KB_MSGQ_PUT`
- `YKB_METRICS_KB_TRANSITION`
- `YKB_METRICS_REPORT_SENT`

Do not call the raw functions directly unless the macro does not fit.

## Important Kconfig

- `CONFIG_YKB_METRICS_KSCAN_UNIT_MAX` - maximum tracked KScan device/unit pairs.
- `CONFIG_YKB_METRICS_LOG_INTERVAL_MS` - reporting interval.

If a KScan unit does not show up, increase `CONFIG_YKB_METRICS_KSCAN_UNIT_MAX`.
