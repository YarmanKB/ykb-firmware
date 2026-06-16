# KScan

KScan is the raw key scanning driver class.

It reads analog key values and reports them through callbacks.

## Public API

```c
// Get the amount of keys handled by the KScan instance.
//
// Returns negative value on failure.
int kscan_get_key_amount(const struct device *dev);

// Get the `idx-offset` property of the KScan instance.
//
// Returns negative value on failure.
int kscan_get_idx_offset(const struct device *dev);

// Get current ADC values of the KScan instance.
// Caller must allocate an array of uint16_t values with the size
// reported by `kscan_get_key_amount`.
//
// Returns 0 on success and negative value otherwise.
int kscan_get_values(const struct device *dev, uint16_t *values);
```

For normal keyboard behavior use callbacks:

```c
KSCAN_CB_DEFINE(name) = {
    .on_new_value = on_new_value,
};
```

The callback receives key index and raw value.

## Implementations

Current implementations:

- `kscan-channels` - polls ADC channels directly.
- `kscan-enables` - uses enable GPIOs around readings.
- `kscan-muxes` - selects mux channels and reads ADC channels.

Multiple KScan devices can exist at the same time. KBHandler consumes all devices listed in `/zephyr,user` `kb-handler-kscans`. This should come in handy when a keyboard uses multiple different ways to scan the keys.

## Indexing

Each KScan device has an `idx-offset`.

The reported key index is:

```text
idx-offset + local-key-index
```

This is needed when multiple KScan instances are used on a single board. So each KScan instance reports its own unique indices.

No need for those offsets be different on split keyboards between master and slave. That should be handled by KBHandler subsystem.
