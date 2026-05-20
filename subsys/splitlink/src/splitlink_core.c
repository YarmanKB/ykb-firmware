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
    STRUCT_SECTION_FOREACH(splitlink_cb, callback) {
        if (callback->connect_cb) {
            callback->connect_cb();
        }
    }
}

void splitlink_notify_disconnected(void) {
    STRUCT_SECTION_FOREACH(splitlink_cb, callback) {
        if (callback->disconnect_cb) {
            callback->disconnect_cb();
        }
    }
}

void splitlink_notify_receive(uint8_t *data, size_t data_len) {
    STRUCT_SECTION_FOREACH(splitlink_cb, callback) {
        if (callback->on_receive_cb) {
            callback->on_receive_cb(data, data_len);
        }
    }
}

int splitlink_send(uint8_t *data, size_t data_len) {
    if (!active_transport) {
        return -ENODEV;
    }

    return active_transport->send(data, data_len);
}
