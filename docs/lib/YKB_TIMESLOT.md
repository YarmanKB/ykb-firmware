# YKB Timeslot

YKB Timeslot is a small helper around MPSL timeslots.

It exists for cases where application ESB and Bluetooth must share the same radio.

## API

```c
typedef enum {
    APP_TS_STARTED,
    APP_TS_STOPPED,
} timeslot_callback_type_t;

void timeslot_handler_init(timeslot_callback_t callback);
```

The callback tells the user when application radio time starts and stops.

## Threading

The library uses a cooperative thread to serialize MPSL timeslot operations.

Stack size:

```conf
CONFIG_LIB_YKB_TIMESLOT_STACK_SIZE=1024
```

This thread should stay small. It is for opening/closing sessions and requesting timeslots, not for application logic.

## Used by

YKB ESB uses this library when ESB needs to coexist with Bluetooth on the same core.

On nRF5340 CPUAPP this is usually not the path, because the radio is on CPUNET.
