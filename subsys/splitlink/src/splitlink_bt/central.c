#include "splitlink_bt.h"

#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>

LOG_MODULE_REGISTER(splitlink_bt_central, CONFIG_SPLITLINK_LOG_LEVEL);

static const struct bt_uuid_128 splitlink_bt_svc_uuid =
    BT_UUID_INIT_128(SPLITLINK_BT_UUID_SVC_VAL);
static const struct bt_uuid_128 splitlink_bt_tx_uuid =
    BT_UUID_INIT_128(SPLITLINK_BT_UUID_TX_VAL);
static const struct bt_uuid_128 splitlink_bt_rx_uuid =
    BT_UUID_INIT_128(SPLITLINK_BT_UUID_RX_VAL);
static const struct bt_le_conn_param *splitlink_bt_conn_params =
    BT_LE_CONN_PARAM(BT_GAP_MS_TO_CONN_INTERVAL(7.5),
                     BT_GAP_MS_TO_CONN_INTERVAL(7.5), 0,
                     BT_GAP_MS_TO_CONN_TIMEOUT(4000));

static int splitlink_bt_send(uint8_t *data, size_t data_len);
static bool bt_callbacks_registered;
static bool splitlink_bt_scan_started;
K_MSGQ_DEFINE(splitlink_bt_rx_msgq, sizeof(struct splitlink_bt_rx_packet),
              CONFIG_SPLITLINK_BT_RX_QUEUE_DEPTH, 4);
static struct splitlink_bt_data splitlink_bt_data = {
    .rx_msgq = &splitlink_bt_rx_msgq,
};
static const struct splitlink_transport_api splitlink_bt_transport_api = {
    .send = splitlink_bt_send,
};

static void splitlink_bt_central_start_scan(void);
static void
splitlink_bt_start_primary_discovery(struct splitlink_bt_data *dev_data);

static void splitlink_bt_request_link_updates(struct bt_conn *conn) {
    int err = bt_conn_le_param_update(conn, splitlink_bt_conn_params);
    if (err && err != -EALREADY) {
        LOG_WRN("Splitlink BT conn param update failed (%d)", err);
    } else {
        LOG_INF("Splitlink BT conn param update requested");
    }

    err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
    if (err && err != -EALREADY) {
        LOG_WRN("Splitlink BT PHY 2M update failed (%d)", err);
    } else {
        LOG_INF("Splitlink BT PHY 2M update requested");
    }

    err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
    if (err && err != -EALREADY) {
        LOG_WRN("Splitlink BT data len update failed (%d)", err);
    } else {
        LOG_INF("Splitlink BT data len update requested");
    }
}

static void splitlink_bt_reset_discovery_state(struct splitlink_bt_data *dev_data) {
    dev_data->notify_enabled = false;
    dev_data->service_start_handle = 0;
    dev_data->service_end_handle = 0;
    dev_data->tx_handle = 0;
    dev_data->rx_handle = 0;
    dev_data->ccc_handle = 0;
    memset(&dev_data->discover_params, 0, sizeof(dev_data->discover_params));
    memset(&dev_data->subscribe_params, 0, sizeof(dev_data->subscribe_params));
}

static void splitlink_bt_mtu_fallback_work_handler(struct k_work *work) {
    struct splitlink_bt_data *dev_data =
        CONTAINER_OF(k_work_delayable_from_work(work), struct splitlink_bt_data,
                     mtu_fallback_work);

    if (!dev_data->conn) {
        return;
    }

    if (dev_data->service_start_handle != 0) {
        return;
    }

    LOG_WRN("Splitlink BT MTU callback timeout, starting discovery anyway");
    splitlink_bt_start_primary_discovery(dev_data);
}

static int splitlink_bt_send(uint8_t *data, size_t data_len) {
    if (!data || data_len == 0) {
        return -EINVAL;
    }

    if (data_len > CONFIG_SPLITLINK_BT_PACKET_LENGTH) {
        return -EMSGSIZE;
    }

    if (!splitlink_bt_data.connected || !splitlink_bt_data.conn ||
        splitlink_bt_data.rx_handle == 0) {
        return -ENOTCONN;
    }

    return bt_gatt_write_without_response(splitlink_bt_data.conn,
                                          splitlink_bt_data.rx_handle, data,
                                          data_len, false);
}

static uint8_t splitlink_bt_notify_cb(struct bt_conn *conn,
                                      struct bt_gatt_subscribe_params *params,
                                      const void *data, uint16_t length) {
    ARG_UNUSED(conn);
    ARG_UNUSED(params);

    struct splitlink_bt_data *dev_data = &splitlink_bt_data;

    if (!data) {
        LOG_WRN("Splitlink BT notifications stopped");
        params->value_handle = 0;
        dev_data->notify_enabled = false;
        dev_data->tx_handle = 0;
        dev_data->ccc_handle = 0;
        dev_data->rx_handle = 0;
        splitlink_bt_notify_disconnected(dev_data);
        return BT_GATT_ITER_STOP;
    }

    int err = splitlink_bt_queue_rx(dev_data, data, length);
    if (err) {
        LOG_ERR("Failed to queue RX notification (%d)", err);
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t
splitlink_bt_discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         struct bt_gatt_discover_params *params) {
    struct splitlink_bt_data *dev_data = &splitlink_bt_data;
    int err;

    if (!attr) {
        LOG_WRN("Splitlink BT discovery ended before completion");
        memset(params, 0, sizeof(*params));
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        const struct bt_gatt_service_val *service = attr->user_data;

        LOG_INF("Splitlink BT service found: start=0x%04x end=0x%04x",
                attr->handle, service->end_handle);
        dev_data->service_start_handle = attr->handle + 1;
        dev_data->service_end_handle = service->end_handle;
        dev_data->discover_params.uuid = &splitlink_bt_tx_uuid.uuid;
        dev_data->discover_params.start_handle = dev_data->service_start_handle;
        dev_data->discover_params.end_handle = dev_data->service_end_handle;
        dev_data->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        err = bt_gatt_discover(conn, &dev_data->discover_params);
        if (err) {
            LOG_ERR("Splitlink BT TX discovery failed (%d)", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }

        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC &&
        !bt_uuid_cmp(params->uuid, &splitlink_bt_tx_uuid.uuid)) {
        dev_data->tx_handle = bt_gatt_attr_value_handle(attr);
        LOG_INF("Splitlink BT TX handle=0x%04x", dev_data->tx_handle);
        dev_data->ccc_handle = dev_data->tx_handle + 1;
        LOG_INF("Splitlink BT CCC handle=0x%04x", dev_data->ccc_handle);
        dev_data->discover_params.uuid = &splitlink_bt_rx_uuid.uuid;
        dev_data->discover_params.start_handle = dev_data->ccc_handle + 1;
        dev_data->discover_params.end_handle = dev_data->service_end_handle;
        dev_data->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        err = bt_gatt_discover(conn, &dev_data->discover_params);
        if (err) {
            LOG_ERR("Splitlink BT RX discovery failed (%d)", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }

        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC &&
        !bt_uuid_cmp(params->uuid, &splitlink_bt_rx_uuid.uuid)) {
        dev_data->rx_handle = bt_gatt_attr_value_handle(attr);
        LOG_INF("Splitlink BT RX handle=0x%04x", dev_data->rx_handle);
        dev_data->subscribe_params.notify = splitlink_bt_notify_cb;
        dev_data->subscribe_params.value = BT_GATT_CCC_NOTIFY;
        dev_data->subscribe_params.value_handle = dev_data->tx_handle;
        dev_data->subscribe_params.ccc_handle = dev_data->ccc_handle;

        err = bt_gatt_subscribe(conn, &dev_data->subscribe_params);
        if (err && err != -EALREADY) {
            LOG_ERR("Splitlink BT subscribe failed (%d)", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return BT_GATT_ITER_STOP;
        }

        LOG_INF("Splitlink BT subscribed");
        dev_data->notify_enabled = true;
        splitlink_bt_notify_connected(dev_data);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

static void splitlink_bt_mtu_exchanged(struct bt_conn *conn, uint8_t err,
                                       struct bt_gatt_exchange_params *params) {
    ARG_UNUSED(params);

    struct splitlink_bt_data *dev_data = &splitlink_bt_data;
    (void)k_work_cancel_delayable(&dev_data->mtu_fallback_work);

    if (err) {
        LOG_WRN("Splitlink BT MTU exchange failed (%u)", err);
    } else {
        LOG_INF("Splitlink BT MTU=%u", bt_gatt_get_mtu(conn));
    }

    splitlink_bt_start_primary_discovery(dev_data);
}

static void
splitlink_bt_start_primary_discovery(struct splitlink_bt_data *dev_data) {
    if (!dev_data->conn) {
        return;
    }

    if (dev_data->service_start_handle != 0) {
        return;
    }

    dev_data->discover_params.uuid = &splitlink_bt_svc_uuid.uuid;
    dev_data->discover_params.func = splitlink_bt_discover_cb;
    dev_data->discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    dev_data->discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    dev_data->discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    LOG_INF("Splitlink BT starting primary service discovery");
    int discover_err =
        bt_gatt_discover(dev_data->conn, &dev_data->discover_params);
    if (discover_err) {
        LOG_ERR("Splitlink BT primary discovery failed (%d)", discover_err);
        bt_conn_disconnect(dev_data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static bool splitlink_bt_match_service(struct bt_data *data, void *user_data) {
    bool *matched = user_data;
    struct bt_uuid_128 adv_uuid;

    if (data->type != BT_DATA_UUID128_ALL &&
        data->type != BT_DATA_UUID128_SOME) {
        return true;
    }

    if (data->data_len % BT_UUID_SIZE_128 != 0) {
        return true;
    }

    for (size_t offset = 0; offset < data->data_len;
         offset += BT_UUID_SIZE_128) {
        if (!bt_uuid_create(&adv_uuid.uuid, data->data + offset,
                            BT_UUID_SIZE_128)) {
            continue;
        }

        if (!bt_uuid_cmp(&adv_uuid.uuid, &splitlink_bt_svc_uuid.uuid)) {
            *matched = true;
            return false;
        }
    }

    return true;
}

static void splitlink_bt_device_found(const bt_addr_le_t *addr, int8_t rssi,
                                      uint8_t type, struct net_buf_simple *ad) {
    bool matched = false;
    int err;

    if (type != BT_GAP_ADV_TYPE_ADV_IND &&
        type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
        type != BT_GAP_ADV_TYPE_SCAN_RSP && type != BT_GAP_ADV_TYPE_EXT_ADV) {
        return;
    }

    if (splitlink_bt_data.conn) {
        return;
    }

    bt_data_parse(ad, splitlink_bt_match_service, &matched);
    if (!matched) {
        return;
    }

    LOG_INF("Splitlink BT service advertisement matched");

    err = bt_le_scan_stop();
    if (err) {
        LOG_WRN("Splitlink BT scan stop failed (%d)", err);
        return;
    }

    err = bt_conn_le_create(
        addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT,
        &splitlink_bt_data.conn);
    if (err) {
        LOG_ERR("Splitlink BT connect failed (%d), restarting scan", err);
        splitlink_bt_data.conn = NULL;
        splitlink_bt_central_start_scan();
        return;
    }

    LOG_INF("Splitlink BT connecting (RSSI %d)", rssi);
}

static void splitlink_bt_central_start_scan(void) {
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    int err = bt_le_scan_start(&scan_param, splitlink_bt_device_found);
    if (err && err != -EALREADY) {
        LOG_ERR("Splitlink BT scan start failed (%d)", err);
    } else {
        splitlink_bt_scan_started = true;
        LOG_INF("Splitlink BT scan started");
    }
}

int splitlink_bt_start(void) {
    struct splitlink_bt_data *dev_data;

    if (!bt_is_ready()) {
        return -EAGAIN;
    }

    dev_data = &splitlink_bt_data;
    dev_data->ready = true;

    if (splitlink_bt_scan_started) {
        return 0;
    }

    splitlink_bt_central_start_scan();
    return 0;
}

static void splitlink_bt_connected(struct bt_conn *conn, uint8_t err) {
    struct splitlink_bt_data *dev_data = &splitlink_bt_data;

    if (conn != dev_data->conn) {
        return;
    }

    LOG_INF("Splitlink BT connected callback: err=%u", err);
    if (err) {
        LOG_WRN("Splitlink BT connect callback error (%u)", err);
        bt_conn_unref(dev_data->conn);
        dev_data->conn = NULL;
        splitlink_bt_central_start_scan();
        return;
    }

    splitlink_bt_request_link_updates(conn);

    dev_data->mtu_exchange_params.func = splitlink_bt_mtu_exchanged;

    LOG_INF("Splitlink BT requesting MTU exchange");
    int gatt_err = bt_gatt_exchange_mtu(conn, &dev_data->mtu_exchange_params);
    if (gatt_err && gatt_err != -EALREADY) {
        LOG_WRN("Splitlink BT MTU request failed (%d)", gatt_err);
        splitlink_bt_mtu_exchanged(conn, 0, &dev_data->mtu_exchange_params);
        return;
    }

    k_work_reschedule(&dev_data->mtu_fallback_work, K_MSEC(500));
}

static void splitlink_bt_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct splitlink_bt_data *dev_data = &splitlink_bt_data;

    if (conn != dev_data->conn) {
        return;
    }

    LOG_WRN("Splitlink BT disconnected: reason=0x%02x", reason);
    (void)k_work_cancel_delayable(&dev_data->mtu_fallback_work);

    if (dev_data->subscribe_params.value_handle != 0) {
        bt_gatt_unsubscribe(conn, &dev_data->subscribe_params);
    }

    splitlink_bt_notify_disconnected(dev_data);
    splitlink_bt_reset_discovery_state(dev_data);

    bt_conn_unref(dev_data->conn);
    dev_data->conn = NULL;

    splitlink_bt_central_start_scan();
}

static struct bt_conn_cb splitlink_bt_conn_cb = {
    .connected = splitlink_bt_connected,
    .disconnected = splitlink_bt_disconnected,
};

static int splitlink_bt_init(void) {
    struct splitlink_bt_data *dev_data = &splitlink_bt_data;
    int err;

    BUILD_ASSERT(CONFIG_BT_L2CAP_TX_MTU >=
                     CONFIG_SPLITLINK_BT_PACKET_LENGTH + 5,
                 "BT_L2CAP_TX_MTU too small for Splitlink BT payload");
    BUILD_ASSERT(CONFIG_BT_BUF_ACL_RX_SIZE >=
                     CONFIG_SPLITLINK_BT_PACKET_LENGTH + 9,
                 "BT_BUF_ACL_RX_SIZE too small for Splitlink BT payload");

    k_work_init(&dev_data->rx_work, splitlink_bt_rx_work_handler);
    k_work_init(&dev_data->connect_work, splitlink_bt_connect_work_handler);
    k_work_init(&dev_data->disconnect_work,
                splitlink_bt_disconnect_work_handler);
    k_work_init_delayable(&dev_data->mtu_fallback_work,
                          splitlink_bt_mtu_fallback_work_handler);

    if (!bt_callbacks_registered) {
        bt_conn_cb_register(&splitlink_bt_conn_cb);
        bt_callbacks_registered = true;
    }

    err = splitlink_register_transport(&splitlink_bt_transport_api);
    if (err) {
        LOG_ERR("Splitlink BT transport register failed (%d)", err);
        return err;
    }

    LOG_INF("Splitlink BT central init ok");

    if (IS_ENABLED(CONFIG_BT_CONNECT)) {
        LOG_INF("Splitlink BT central waiting for bt_connect");
        return 0;
    }

    err = bt_enable(NULL);
    if (err && err != -EALREADY) {
        LOG_ERR("Splitlink BT bt_enable failed (%d)", err);
        return err;
    }

    return splitlink_bt_start();
}

static int splitlink_bt_sys_init(void) { return splitlink_bt_init(); }

SYS_INIT(splitlink_bt_sys_init, POST_KERNEL, CONFIG_SPLITLINK_BT_INIT_PRIORITY);
