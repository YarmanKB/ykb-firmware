#ifndef __DRIVERS_SPLITLINK_H_
#define __DRIVERS_SPLITLINK_H_

#include <zephyr/device.h>
#include <zephyr/toolchain.h>

#if CONFIG_SPLITLINK_BT
#define SPLITLINK_MAX_PACKET_LENGTH CONFIG_SPLITLINK_BT_PACKET_LENGTH
#elif CONFIG_SPLITLINK_YKB_ESB
#define SPLITLINK_MAX_PACKET_LENGTH (CONFIG_ESB_MAX_PAYLOAD_LENGTH - 1)
#endif // CONFIG_SPLITLINK_BT / CONFIG_SPLITLINK_YKB_ESB

#ifndef SPLITLINK_MAX_PACKET_LENGTH
#error SPLITLINK_MAX_PACKET_LENGTH is not defined for the current SplitLink driver
#endif // SPLITLINK_MAX_PACKET_LENGTH

struct splitlink_cb {
    void (*on_receive_cb)(const struct device *dev, uint8_t *data,
                          size_t data_len);
    void (*connect_cb)(const struct device *dev);
    void (*disconnect_cb)(const struct device *dev);
};

#define SPLITLINK_CB_DEFINE(name)                                              \
    static STRUCT_SECTION_ITERABLE(splitlink_cb, __splitlink_cb_##name)

__subsystem struct splitlink_driver_api {
    int (*send)(const struct device *dev, uint8_t *data, size_t data_len);
};

__syscall int splitlink_send(const struct device *dev, uint8_t *data,
                             size_t data_len);

static inline int z_impl_splitlink_send(const struct device *dev, uint8_t *data,
                                        size_t data_len) {
    __ASSERT_NO_MSG(DEVICE_API_IS(splitlink, dev));
    return DEVICE_API_GET(splitlink, dev)->send(dev, data, data_len);
}

#include <syscalls/splitlink.h>

#endif // __DRIVERS_SPLITLINK_H_
