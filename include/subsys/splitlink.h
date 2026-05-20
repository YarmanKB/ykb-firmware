#ifndef __SUBSYS_SPLITLINK_H_
#define __SUBSYS_SPLITLINK_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

#if CONFIG_SPLITLINK_BT
#define SPLITLINK_MAX_PACKET_LENGTH CONFIG_SPLITLINK_BT_PACKET_LENGTH
#elif CONFIG_SPLITLINK_YKB_ESB
#define SPLITLINK_MAX_PACKET_LENGTH (CONFIG_ESB_MAX_PAYLOAD_LENGTH - 1)
#endif

#ifndef SPLITLINK_MAX_PACKET_LENGTH
#error SPLITLINK_MAX_PACKET_LENGTH is not defined for the current SplitLink transport
#endif

struct splitlink_cb {
    void (*on_receive_cb)(uint8_t *data, size_t data_len);
    void (*connect_cb)(void);
    void (*disconnect_cb)(void);
};

#define SPLITLINK_CB_DEFINE(name)                                              \
    static STRUCT_SECTION_ITERABLE(splitlink_cb, __splitlink_cb_##name)

struct splitlink_transport_api {
    int (*send)(uint8_t *data, size_t data_len);
};

int splitlink_register_transport(const struct splitlink_transport_api *api);
bool splitlink_is_ready(void);
void splitlink_notify_connected(void);
void splitlink_notify_disconnected(void);
void splitlink_notify_receive(uint8_t *data, size_t data_len);
int splitlink_send(uint8_t *data, size_t data_len);

#endif // __SUBSYS_SPLITLINK_H_
