#ifndef __DRIVERS_KSCAN_H_
#define __DRIVERS_KSCAN_H_

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/toolchain.h>

struct kscan_cb {
    void (*on_new_value)(uint16_t index, uint16_t value);
};

#define KSCAN_CB_DEFINE(name)                                                  \
    static STRUCT_SECTION_ITERABLE(kscan_cb, __kscan_cb__##name)

__subsystem struct kscan_driver_api {
    int (*get_key_amount)(const struct device *dev);
    int (*get_idx_offset)(const struct device *dev);
    int (*get_values)(const struct device *dev, uint16_t *values);
};

// Get the amount of keys managed by the KScan instance.
//
// Returns key_amount on success, negative value otherwise
__syscall int kscan_get_key_amount(const struct device *dev);

// Get the index offset of the KScan instance.
// The value which KScan adds to the key index to report to KScan callbacks.
//
// Returns idx_offset on success, negative value otherwise
__syscall int kscan_get_idx_offset(const struct device *dev);

// Get current ADC values for each managed key.
// Caller must allocate values array of size get_key_amount().
// It is genuinely better to use KSCAN_CB_DEFINE on_new_value callback instead
// to not worry about managing KScan instances.
//
// Returns 0 on success, negative value otherwise
__syscall int kscan_get_values(const struct device *dev, uint16_t *values);

static inline int z_impl_kscan_get_key_amount(const struct device *dev) {
    __ASSERT_NO_MSG(DEVICE_API_GET(kscan, dev));
    return DEVICE_API_GET(kscan, dev)->get_key_amount(dev);
}

static inline int z_impl_kscan_get_idx_offset(const struct device *dev) {
    __ASSERT_NO_MSG(DEVICE_API_GET(kscan, dev));
    return DEVICE_API_GET(kscan, dev)->get_idx_offset(dev);
}

static inline int z_impl_kscan_get_values(const struct device *dev,
                                          uint16_t *values) {
    __ASSERT_NO_MSG(DEVICE_API_GET(kscan, dev));
    return DEVICE_API_GET(kscan, dev)->get_values(dev, values);
}

#include <syscalls/kscan.h>

#endif // __DRIVERS_KSCAN_H_
