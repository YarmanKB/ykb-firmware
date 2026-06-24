#ifndef __DRIVERS_KSCAN_H_
#define __DRIVERS_KSCAN_H_

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/toolchain.h>

__subsystem struct kscan_driver_api {
    int (*get_key_amount)(const struct device *dev);
    int (*get_idx_offset)(const struct device *dev);
    int (*scan)(const struct device *dev, uint16_t *values);
};

// Get the amount of keys managed by the KScan instance.
//
// Returns key_amount on success, negative value otherwise.
__syscall int kscan_get_key_amount(const struct device *dev);

// Get the index offset of the KScan instance.
// The value which KScan adds to the key index to report to KScan callbacks.
//
// Returns idx_offset on success, negative value otherwise.
__syscall int kscan_get_idx_offset(const struct device *dev);

// Perform scanning on KScan instance.
// The caller needs to allocate the array 'values'
// with capacity returned by kscan_get_key_amount.
//
// Returns 0 on success, negative value otherwise
__syscall int kscan_scan(const struct device *dev, uint16_t *values);

static inline int z_impl_kscan_get_key_amount(const struct device *dev) {
    __ASSERT_NO_MSG(DEVICE_API_GET(kscan, dev));
    return DEVICE_API_GET(kscan, dev)->get_key_amount(dev);
}

static inline int z_impl_kscan_get_idx_offset(const struct device *dev) {
    __ASSERT_NO_MSG(DEVICE_API_GET(kscan, dev));
    return DEVICE_API_GET(kscan, dev)->get_idx_offset(dev);
}

static inline int z_impl_kscan_scan(const struct device *dev,
                                    uint16_t *values) {
    __ASSERT_NO_MSG(DEVICE_API_GET(kscan, dev));
    return DEVICE_API_GET(kscan, dev)->scan(dev, values);
}

#include <syscalls/kscan.h>

#endif // __DRIVERS_KSCAN_H_
