#include "sync/splitlink_sync_private.h"
#include <subsys/splitlink.h>

#include <zephyr/kernel.h>

static const struct splitlink_transport_api *active_transport;

int splitlink_register_transport(const struct splitlink_transport_api *api) {
    if (!api || !api->send) {
        return -EINVAL;
    }

    if (active_transport && active_transport != api) {
        return -EALREADY;
    }

    active_transport = api;
    return 0;
}

bool splitlink_is_ready(void) { return active_transport != NULL; }

void splitlink_notify_connected(void) {
#if CONFIG_SPLITLINK_SYNC
    splitlink_sync_protocol_on_connect();
#endif // CONFIG_SPLITLINK_SYNC
}

void splitlink_notify_disconnected(void) {
#if CONFIG_SPLITLINK_SYNC
    splitlink_sync_protocol_on_disconnect();
#endif // CONFIG_SPLITLINK_SYNC
}

void splitlink_notify_receive(uint8_t *data, size_t data_len) {
#if CONFIG_SPLITLINK_SYNC
    splitlink_sync_protocol_on_receive(data, data_len);
#endif // CONFIG_SPLITLINK_SYNC
}

int splitlink_send(uint8_t *data, size_t data_len) {
    if (!active_transport) {
        return -ENODEV;
    }

    return active_transport->send(data, data_len);
}
