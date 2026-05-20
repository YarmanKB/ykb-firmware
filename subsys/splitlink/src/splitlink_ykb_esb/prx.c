#include "splitlink_esb.h"

LOG_MODULE_REGISTER(splitlink_esb_prx, CONFIG_SPLITLINK_LOG_LEVEL);

#define SPLITLINK_ESB_INIT_DELAY_MS 5000
#define SPLITLINK_ESB_RETRY_DELAY_MS 2000

static struct splitlink_data splitlink_esb_data = {
    .connected = false,
};
static const struct splitlink_config splitlink_esb_config = {
    .esb_default_address = {generated_splitlink_esb_address[0],
                            generated_splitlink_esb_address[1],
                            generated_splitlink_esb_address[2],
                            generated_splitlink_esb_address[3],
                            generated_splitlink_esb_address[4],
                            generated_splitlink_esb_address[5],
                            generated_splitlink_esb_address[6],
                            generated_splitlink_esb_address[7]},
};
static int splitlink_ykb_esb_send(uint8_t *data, size_t data_len);
static const struct splitlink_transport_api splitlink_esb_transport_api = {
    .send = splitlink_ykb_esb_send,
};

static int splitlink_ykb_esb_send(uint8_t *data, size_t data_len) {
    if (data_len == 0 || data == NULL) {
        LOG_ERR("Invalid argument.");
        return -EINVAL;
    }
    if (data_len > CONFIG_ESB_MAX_PAYLOAD_LENGTH - 1) {
        LOG_ERR("Packet length is too high (%u > %u)", data_len,
                CONFIG_ESB_MAX_PAYLOAD_LENGTH - 1);
        return -EINVAL;
    }

    ykb_esb_data_t packet = {
        .len = data_len + 1,
    };
    memcpy(&packet.data[1], data, data_len);
    packet.data[0] = FLAG_DATA;

    return ykb_esb_send(&packet);
}

static void connect_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    splitlink_notify_connected();
}

static void disconnect_work_handler(struct k_work *work) {
    // Oopsie, we got a disconnect...
    struct delayable_work_ctx *dev_work =
        CONTAINER_OF(work, struct delayable_work_ctx, d_work.work);
    ARG_UNUSED(dev_work);

    struct splitlink_data *data = &splitlink_esb_data;
    data->connected = false;
    splitlink_notify_disconnected();
}

static void receiving_work_handler(struct k_work *work) {
    struct receiving_work_ctx *dev_work =
        CONTAINER_OF(work, struct receiving_work_ctx, work);
    splitlink_notify_receive(dev_work->data, dev_work->data_len);
}

static void on_esb_callback(ykb_esb_event_t *event, void *user_ptr) {
    ARG_UNUSED(user_ptr);
    struct splitlink_data *dev_data = &splitlink_esb_data;
    if (event->evt_type == YKB_ESB_EVT_RX) {
        // If not connected, we got a connection now
        if (!dev_data->connected) {
            dev_data->connected = true;
            k_work_submit(&dev_data->connect_work.work);
        }
        if (event->data_length > 1 && event->buf[0] == FLAG_DATA) {
            dev_data->receiving_work.data_len = event->data_length - 1;
            memcpy(dev_data->receiving_work.data, &event->buf[1],
                   event->data_length - 1);
            k_work_submit(&dev_data->receiving_work.work);
        }
        // Cancel disconnect work if it was scheduled before
        k_work_cancel_delayable(&dev_data->disconnect_work.d_work);
        // And schedule it again
        k_work_schedule(&dev_data->disconnect_work.d_work,
                        K_MSEC(CONFIG_SPLITLINK_YKB_ESB_ALIVE_TIMEOUT));
    }
}

static void init_work_handler(struct k_work *work) {
    struct delayable_work_ctx *init_work =
        CONTAINER_OF(work, struct delayable_work_ctx, d_work.work);
    ARG_UNUSED(init_work);
    const struct splitlink_config *cfg = &splitlink_esb_config;
    struct splitlink_data *data = &splitlink_esb_data;

    ykb_esb_config_t esb_cfg = {
        .mode = YKB_ESB_MODE_PRX,
        .user_ptr = NULL,
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
        LOG_INF("Init work handler OK");
        data->ready = true;
    }
}

static int splitlink_esb_init(void) {
    struct splitlink_data *data = &splitlink_esb_data;

    // Don't know how to fix that yet, but for some reason ESB only
    // initializes after some delay and panics somewhere in rpmsg_virtio
    k_work_init_delayable(&data->init_work.d_work, init_work_handler);
    k_work_schedule(&data->init_work.d_work,
                    K_MSEC(SPLITLINK_ESB_INIT_DELAY_MS));

    k_work_init_delayable(&data->disconnect_work.d_work,
                          disconnect_work_handler);
    k_work_init(&data->connect_work.work, connect_work_handler);
    k_work_init(&data->receiving_work.work, receiving_work_handler);
    return splitlink_register_transport(&splitlink_esb_transport_api);
}

static int splitlink_esb_sys_init(void) { return splitlink_esb_init(); }

SYS_INIT(splitlink_esb_sys_init, POST_KERNEL, CONFIG_SPLITLINK_YKB_ESB_INIT_PRIORITY);
