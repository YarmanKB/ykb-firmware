#ifndef SPLITLINK_BT_H_
#define SPLITLINK_BT_H_

#include <subsys/splitlink.h>

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#define SPLITLINK_BT_UUID_SVC_VAL                                              \
    BT_UUID_128_ENCODE(0x69d61ea1, 0xd68f, 0x49cc, 0x9f6a, 0x4a4aeb7e1000)
#define SPLITLINK_BT_UUID_TX_VAL                                               \
    BT_UUID_128_ENCODE(0x69d61ea1, 0xd68f, 0x49cc, 0x9f6a, 0x4a4aeb7e1001)
#define SPLITLINK_BT_UUID_RX_VAL                                               \
    BT_UUID_128_ENCODE(0x69d61ea1, 0xd68f, 0x49cc, 0x9f6a, 0x4a4aeb7e1002)

struct splitlink_bt_rx_packet {
    uint8_t len;
    uint8_t data[CONFIG_SPLITLINK_BT_PACKET_LENGTH];
};

struct splitlink_bt_config {};

struct splitlink_bt_data {
    struct bt_conn *conn;
    struct k_work rx_work;
    struct k_work connect_work;
    struct k_work disconnect_work;
    struct k_work_delayable mtu_fallback_work;
    struct k_work_delayable adv_restart_work;
    struct k_msgq *rx_msgq;
    bool connected;
    bool ready;
    bool notify_enabled;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t tx_handle;
    uint16_t rx_handle;
    uint16_t ccc_handle;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
    struct bt_gatt_exchange_params mtu_exchange_params;
};

static inline void
splitlink_bt_notify_connected(struct splitlink_bt_data *data) {
    if (data->connected) {
        return;
    }

    data->connected = true;
    k_work_submit(&data->connect_work);
}

static inline void
splitlink_bt_notify_disconnected(struct splitlink_bt_data *data) {
    if (!data->connected) {
        return;
    }

    data->connected = false;
    k_work_submit(&data->disconnect_work);
}

static inline void splitlink_bt_connect_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    splitlink_notify_connected();
}

static inline void splitlink_bt_disconnect_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    splitlink_notify_disconnected();
}

static inline void splitlink_bt_rx_work_handler(struct k_work *work) {
    struct splitlink_bt_data *data =
        CONTAINER_OF(work, struct splitlink_bt_data, rx_work);
    struct splitlink_bt_rx_packet packet;

    while (k_msgq_get(data->rx_msgq, &packet, K_NO_WAIT) == 0) {
        splitlink_notify_receive(packet.data, packet.len);
    }
}

static inline int splitlink_bt_queue_rx(struct splitlink_bt_data *data,
                                        const uint8_t *buf, uint16_t len) {
    struct splitlink_bt_rx_packet packet = {
        .len = len,
    };

    if (len == 0 || len > sizeof(packet.data)) {
        return -EMSGSIZE;
    }

    memcpy(packet.data, buf, len);

    int err = k_msgq_put(data->rx_msgq, &packet, K_NO_WAIT);
    if (err) {
        return err;
    }

    k_work_submit(&data->rx_work);
    return 0;
}

#endif // SPLITLINK_BT_H_
