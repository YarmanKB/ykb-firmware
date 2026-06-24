# KScan

KScan is the raw key scanning driver class.

It reads analog key values into caller-provided scan buffers.

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

// Run one full scan and write ADC values of the KScan instance.
// Caller must allocate an array of uint16_t values with the size
// reported by `kscan_get_key_amount`.
//
// Returns 0 on success and negative value otherwise.
int kscan_scan(const struct device *dev, uint16_t *values);
```

For normal keyboard behavior, KBHandler calls `kscan_scan()` for every device
listed in `/zephyr,user` `kb-handler-kscans`.

## Implementations

Current implementations:

- `kscan-muxes` - selects mux channels and reads ADC channels.

Multiple KScan devices can exist at the same time. KBHandler consumes all devices listed in `/zephyr,user` `kb-handler-kscans`. This should come in handy when a keyboard uses multiple different ways to scan the keys.

## Indexing

Each KScan device has an `idx-offset`.

The reported key index is:

```text
idx-offset + local-key-index
```

This is needed when multiple KScan instances are used on a single board. So each KScan instance writes into its own unique range in the local scan frame.

No need for those offsets be different on split keyboards between master and slave. That should be handled by KBHandler subsystem.
