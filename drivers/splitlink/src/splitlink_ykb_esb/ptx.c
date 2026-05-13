#define DT_DRV_COMPAT splitlink_ykb_esb_ptx

#include "splitlink_esb.h"

LOG_MODULE_REGISTER(splitlink_esb_ptx, CONFIG_SPLITLINK_LOG_LEVEL);

#define SPLITLINK_ESB_INIT_DELAY_MS 100
#define SPLITLINK_ESB_RETRY_DELAY_MS 100

static int splitlink_ykb_esb_send(const struct device *dev, uint8_t *data,
                                  size_t data_len) {
    LOG_INF("SEND");
    if (data_len == 0 || data == NULL) {
        LOG_ERR("Invalid argument.");
        return -EINVAL;
    }
    if (data_len > CONFIG_ESB_MAX_PAYLOAD_LENGTH - 1) {
        LOG_ERR("Packet length is too high (%u > %u)", data_len,
                CONFIG_ESB_MAX_PAYLOAD_LENGTH - 1);
        return -EINVAL;
    }

    struct splitlink_data *dev_data = dev->data;
    if (!dev_data->ready) {
        LOG_DBG("Not ready");
        return -EBUSY;
    }
    if (!dev_data->connected) {
        LOG_ERR("Not connected");
        return -EBUSY;
    }

    ykb_esb_data_t packet = {
        .len = data_len + 1,
    };
    memcpy(&packet.data[1], data, data_len);
    packet.data[0] = FLAG_DATA;

    int err = ykb_esb_send(&packet);
    if (!err) {
        // Hopefully the packet got sent, we need to delay alive packets.
        // If alive work is pending then we cancel it and reschedule
        if (k_work_delayable_is_pending(&dev_data->alive_work.d_work)) {
            k_work_cancel_delayable(&dev_data->alive_work.d_work);
            k_work_schedule(&dev_data->alive_work.d_work,
                            K_MSEC(CONFIG_SPLITLINK_YKB_ESB_PTX_ALIVE_DELAY));
        }
    }

    return err;
}

static void alive_work_handler(struct k_work *work) {
    LOG_INF("ALIVE HANDLER");
    struct delayable_device_work *alive_work =
        CONTAINER_OF(work, struct delayable_device_work, d_work.work);
    ykb_esb_data_t alive_data = {
        .data = {FLAG_ALIVE},
        .len = 1,
    };
    struct splitlink_data *data = alive_work->dev->data;
    if (!data->ready) {
        goto schedule;
    }
    int err = ykb_esb_send(&alive_data);
    if (err) {
        LOG_ERR("Unable to send alive packet: %d", err);
    } else {
        LOG_INF("TX alive");
    }
schedule:
    // Schedule itself
    k_work_schedule(&alive_work->d_work,
                    K_MSEC(CONFIG_SPLITLINK_YKB_ESB_PTX_ALIVE_DELAY));
}

static void connect_work_handler(struct k_work *work) {
    struct device_work *dev_work = CONTAINER_OF(work, struct device_work, work);

    // We set the 'connected' before calling this work

    STRUCT_SECTION_FOREACH(splitlink_cb, callback) {
        if (callback->connect_cb) {
            callback->connect_cb(dev_work->dev);
        }
    }
}

static void disconnect_work_handler(struct k_work *work) {
    // Oopsie, we got a disconnect...
    struct delayable_device_work *dev_work =
        CONTAINER_OF(work, struct delayable_device_work, d_work.work);

    struct splitlink_data *data = dev_work->dev->data;
    data->connected = false;

    STRUCT_SECTION_FOREACH(splitlink_cb, callback) {
        if (callback->disconnect_cb) {
            callback->disconnect_cb(dev_work->dev);
        }
    }
}

static void receiving_work_handler(struct k_work *work) {
    struct receiving_device_work *dev_work =
        CONTAINER_OF(work, struct receiving_device_work, work);

    STRUCT_SECTION_FOREACH(splitlink_cb, callback) {
        if (callback->on_receive_cb) {
            callback->on_receive_cb(dev_work->dev, dev_work->data,
                                    dev_work->data_len);
        }
    }
}

static void on_esb_callback(ykb_esb_event_t *event, void *user_ptr) {
    const struct device *dev = user_ptr;
    struct splitlink_data *dev_data = dev->data;
    LOG_INF("evt %d", event->evt_type);
    if (event->evt_type == YKB_ESB_EVT_RX && event->data_length > 2 &&
        event->buf[0] == FLAG_DATA) {
        dev_data->receiving_work.data_len = event->data_length - 1;
        memcpy(dev_data->receiving_work.data, &event->buf[1],
               event->data_length - 1);
        k_work_submit(&dev_data->receiving_work.work);
    } else if (event->evt_type == YKB_ESB_EVT_TX_SUCCESS) {
        // If not connected, we got a connection now
        if (!dev_data->connected) {
            dev_data->connected = true;
            k_work_submit(&dev_data->connect_work.work);
        }
        // Cancel disconnect work if it was scheduled before
        k_work_cancel_delayable(&dev_data->disconnect_work.d_work);
        // And schedule it again
        k_work_schedule(&dev_data->disconnect_work.d_work,
                        K_MSEC(CONFIG_SPLITLINK_YKB_ESB_ALIVE_TIMEOUT));
        LOG_INF("TX success");
    }
    if (event->evt_type == YKB_ESB_EVT_TX_FAIL) {
        LOG_INF("TX fail");
    }
}

static void init_work_handler(struct k_work *work) {
    struct delayable_device_work *init_work =
        CONTAINER_OF(work, struct delayable_device_work, d_work.work);
    const struct splitlink_config *cfg = init_work->dev->config;
    struct splitlink_data *data = init_work->dev->data;

    ykb_esb_config_t esb_cfg = {
        .mode = YKB_ESB_MODE_PTX,
        .user_ptr = (void *)init_work->dev,
    };
    memcpy(esb_cfg.base_addr_0, cfg->esb_default_address,
           sizeof(esb_cfg.base_addr_0));
    memcpy(esb_cfg.base_addr_1, &cfg->esb_default_address[4],
           sizeof(esb_cfg.base_addr_1));

    int err = ykb_esb_init(&esb_cfg, on_esb_callback);
    if (err) {
        LOG_WRN("YKB ESB init deferred (%d), retrying", err);
        k_work_schedule(&data->init_work.d_work,
                        K_MSEC(SPLITLINK_ESB_RETRY_DELAY_MS));
    } else {
        data->ready = true;
        LOG_INF("Init work handler OK");
    }
}

static int splitlink_esb_init(const struct device *dev) {
    struct splitlink_data *data = dev->data;

    data->init_work.dev = dev;
    data->alive_work.dev = dev;
    data->connect_work.dev = dev;
    data->disconnect_work.dev = dev;
    data->receiving_work.dev = dev;

    // Don't know how to fix that yet, but for some reason ESB only
    // initializes after some delay and panics somewhere in rpmsg_virtio
    k_work_init_delayable(&data->init_work.d_work, init_work_handler);
    k_work_schedule(&data->init_work.d_work,
                    K_MSEC(SPLITLINK_ESB_INIT_DELAY_MS));

    k_work_init_delayable(&data->disconnect_work.d_work,
                          disconnect_work_handler);
    k_work_init(&data->connect_work.work, connect_work_handler);
    k_work_init(&data->receiving_work.work, receiving_work_handler);

    k_work_init_delayable(&data->alive_work.d_work, alive_work_handler);
    k_work_schedule(&data->alive_work.d_work,
                    K_MSEC(CONFIG_SPLITLINK_YKB_ESB_PTX_ALIVE_DELAY));

    return 0;
}

DEVICE_API(splitlink, splitlink_esb_api) = {
    .send = splitlink_ykb_esb_send,
};

#define SPLITLINK_YKB_ESB_PTX_DEFINE(inst)                                     \
    static const struct splitlink_config                                       \
        __splitlink_ykb_esb_ptx_config__##inst = {                             \
            .esb_default_address = {generated_splitlink_esb_address[0],        \
                                    generated_splitlink_esb_address[1],        \
                                    generated_splitlink_esb_address[2],        \
                                    generated_splitlink_esb_address[3],        \
                                    generated_splitlink_esb_address[4],        \
                                    generated_splitlink_esb_address[5],        \
                                    generated_splitlink_esb_address[6],        \
                                    generated_splitlink_esb_address[7]},       \
    };                                                                         \
    static struct splitlink_data __splitlink_ykb_esb_ptx_data__##inst = {      \
        .connected = false,                                                    \
    };                                                                         \
    DEVICE_DT_INST_DEFINE(                                                     \
        inst, splitlink_esb_init, NULL, &__splitlink_ykb_esb_ptx_data__##inst, \
        &__splitlink_ykb_esb_ptx_config__##inst, POST_KERNEL,                  \
        CONFIG_SPLITLINK_YKB_ESB_INIT_PRIORITY, &splitlink_esb_api);

DT_INST_FOREACH_STATUS_OKAY(SPLITLINK_YKB_ESB_PTX_DEFINE)
