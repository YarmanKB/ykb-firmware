#define DT_DRV_COMPAT splitlink_bt_peripheral

#include "splitlink_bt.h"

LOG_MODULE_REGISTER(splitlink_bt_peripheral, CONFIG_SPLITLINK_LOG_LEVEL);

static const struct bt_uuid_128 splitlink_bt_svc_uuid =
    BT_UUID_INIT_128(SPLITLINK_BT_UUID_SVC_VAL);
static const struct bt_uuid_128 splitlink_bt_tx_uuid =
    BT_UUID_INIT_128(SPLITLINK_BT_UUID_TX_VAL);
static const struct bt_uuid_128 splitlink_bt_rx_uuid =
    BT_UUID_INIT_128(SPLITLINK_BT_UUID_RX_VAL);

static const struct bt_data splitlink_bt_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, SPLITLINK_BT_UUID_SVC_VAL),
};

static const struct bt_data splitlink_bt_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, SPLITLINK_BT_UUID_SVC_VAL),
};

static const struct device *splitlink_bt_dev;
static bool bt_callbacks_registered;

static void splitlink_bt_peripheral_start_advertising(void);
static void splitlink_bt_adv_restart_work_handler(struct k_work *work) {
    struct splitlink_bt_data *dev_data =
        CONTAINER_OF(k_work_delayable_from_work(work), struct splitlink_bt_data,
                     adv_restart_work);

    if (dev_data->conn) {
        return;
    }

    splitlink_bt_peripheral_start_advertising();
}

static void splitlink_bt_ccc_cfg_changed(const struct bt_gatt_attr *attr,
                                         uint16_t value) {
    ARG_UNUSED(attr);

    struct splitlink_bt_data *dev_data = splitlink_bt_dev->data;

    LOG_INF("Splitlink BT CCC changed: value=0x%04x", value);
    dev_data->notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    if (dev_data->notify_enabled && dev_data->conn) {
        splitlink_bt_notify_connected(dev_data);
    } else {
        splitlink_bt_notify_disconnected(dev_data);
    }
}

static ssize_t splitlink_bt_rx_write(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     const void *buf, uint16_t len,
                                     uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len == 0 || len > CONFIG_SPLITLINK_BT_PACKET_LENGTH) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct splitlink_bt_data *dev_data = splitlink_bt_dev->data;
    int err = splitlink_bt_queue_rx(dev_data, buf, len);
    if (err) {
        LOG_ERR("Failed to queue RX write (%d)", err);
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

    return len;
}

BT_GATT_SERVICE_DEFINE(
    splitlink_bt_svc, BT_GATT_PRIMARY_SERVICE(&splitlink_bt_svc_uuid.uuid),
    BT_GATT_CHARACTERISTIC(&splitlink_bt_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(splitlink_bt_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&splitlink_bt_rx_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, splitlink_bt_rx_write,
                           NULL));

static int splitlink_bt_send(const struct device *dev, uint8_t *data,
                             size_t data_len) {
    struct splitlink_bt_data *dev_data = dev->data;

    if (!data || data_len == 0) {
        return -EINVAL;
    }

    if (data_len > CONFIG_SPLITLINK_BT_PACKET_LENGTH) {
        return -EMSGSIZE;
    }

    if (!dev_data->connected || !dev_data->conn || !dev_data->notify_enabled) {
        return -ENOTCONN;
    }

    return bt_gatt_notify(dev_data->conn, &splitlink_bt_svc.attrs[2], data,
                          data_len);
}

static void splitlink_bt_connected(struct bt_conn *conn, uint8_t err) {
    struct splitlink_bt_data *dev_data = splitlink_bt_dev->data;

    LOG_INF("Splitlink BT peripheral connected callback: err=%u", err);
    if (err) {
        LOG_WRN("Splitlink BT peripheral connect error (%u)", err);
        (void)k_work_reschedule(&dev_data->adv_restart_work, K_MSEC(200));
        return;
    }

    if (dev_data->conn) {
        bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }

    dev_data->conn = bt_conn_ref(conn);
    LOG_INF("Splitlink BT peripheral connected");
}

static void splitlink_bt_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct splitlink_bt_data *dev_data = splitlink_bt_dev->data;

    if (conn != dev_data->conn) {
        return;
    }

    LOG_WRN("Splitlink BT peripheral disconnected: reason=0x%02x", reason);
    splitlink_bt_notify_disconnected(dev_data);
    dev_data->notify_enabled = false;
    bt_conn_unref(dev_data->conn);
    dev_data->conn = NULL;
    (void)k_work_reschedule(&dev_data->adv_restart_work, K_MSEC(200));
}

static struct bt_conn_cb splitlink_bt_conn_cb = {
    .connected = splitlink_bt_connected,
    .disconnected = splitlink_bt_disconnected,
};

static void splitlink_bt_peripheral_start_advertising(void) {
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, splitlink_bt_ad,
                              ARRAY_SIZE(splitlink_bt_ad), splitlink_bt_sd,
                              ARRAY_SIZE(splitlink_bt_sd));
    if (err && err != -EALREADY) {
        LOG_ERR("Splitlink BT advertising failed (%d)", err);
        if (err == -ENOMEM) {
            (void)k_work_reschedule(
                &((struct splitlink_bt_data *)splitlink_bt_dev->data)
                     ->adv_restart_work,
                K_MSEC(200));
        }
    } else {
        LOG_INF("Splitlink BT advertising started");
    }
}

static int splitlink_bt_init(const struct device *dev) {
    struct splitlink_bt_data *dev_data = dev->data;
    int err;

    dev_data->dev = dev;
    splitlink_bt_dev = dev;

    k_work_init(&dev_data->rx_work, splitlink_bt_rx_work_handler);
    k_work_init(&dev_data->connect_work, splitlink_bt_connect_work_handler);
    k_work_init(&dev_data->disconnect_work,
                splitlink_bt_disconnect_work_handler);
    k_work_init_delayable(&dev_data->adv_restart_work,
                          splitlink_bt_adv_restart_work_handler);

    if (!bt_callbacks_registered) {
        bt_conn_cb_register(&splitlink_bt_conn_cb);
        bt_callbacks_registered = true;
    }

    err = bt_enable(NULL);
    if (err && err != -EALREADY) {
        LOG_ERR("Splitlink BT bt_enable failed (%d)", err);
        return err;
    }

    dev_data->ready = true;
    LOG_INF("Splitlink BT peripheral init ok");
    splitlink_bt_peripheral_start_advertising();
    return 0;
}

static DEVICE_API(splitlink, splitlink_bt_peripheral_api) = {
    .send = splitlink_bt_send,
};

#define SPLITLINK_BT_PERIPHERAL_DEFINE(inst)                                   \
    K_MSGQ_DEFINE(__splitlink_bt_rx_msgq_##inst,                               \
                  sizeof(struct splitlink_bt_rx_packet),                       \
                  CONFIG_SPLITLINK_BT_RX_QUEUE_DEPTH, 4);                      \
    static const struct splitlink_bt_config __splitlink_bt_config_##inst = {}; \
    static struct splitlink_bt_data __splitlink_bt_data_##inst = {             \
        .rx_msgq = &__splitlink_bt_rx_msgq_##inst,                             \
    };                                                                         \
    DEVICE_DT_INST_DEFINE(                                                     \
        inst, splitlink_bt_init, NULL, &__splitlink_bt_data_##inst,            \
        &__splitlink_bt_config_##inst, POST_KERNEL,                            \
        CONFIG_SPLITLINK_BT_INIT_PRIORITY, &splitlink_bt_peripheral_api);

DT_INST_FOREACH_STATUS_OKAY(SPLITLINK_BT_PERIPHERAL_DEFINE)
