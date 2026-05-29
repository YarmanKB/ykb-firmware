#include <subsys/vendor_hid_protocol.h>

#include <subsys/kb_handler.h>
#include <subsys/splitlink_sync.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

// TODO: THIS WILL HAVE ISSUES when both USBConnect and BTConnect subsystems
// will use this simultaneously. Probably fine for now. I hope.

LOG_MODULE_REGISTER(vendor_hid_protocol, LOG_LEVEL_INF);

static FEATURES_DEFINE(features);
static kb_settings_t settings_snap;

#if CONFIG_YKB_BACKLIGHT
static vendor_hid_proto_script_slot_packet_t script_slot_snap;
static vendor_hid_proto_script_slot_info_t script_slot_info_snap;
#endif // CONFIG_YKB_BACKLIGHT

#if CONFIG_YKB_BATTSENSE
static ykb_battsense_state_t batt_state;
#endif // CONFIG_YKB_BATTSENSE

#if CONFIG_SPLITLINK_SYNC_MASTER
static ykb_battsense_state_t secondary_batt_state = {0};
static K_MUTEX_DEFINE(secondary_batt_state_mut);
#endif // CONFIG_SPLITLINK_SYNC_MASTER

static kb_settings_t *vendor_hid_protocol_get_settings(void) {
    int err = kb_settings_get(&settings_snap);
    if (err) {
        LOG_ERR("kb_settings_get: %d", err);
        return NULL;
    }

    return &settings_snap;
}

static int vendor_hid_protocol_set_settings(const kb_settings_t *settings) {
    int err = kb_settings_apply(settings);
    if (err) {
        LOG_ERR("kb_settings_apply: %d", err);
    }

    return err;
}

#if CONFIG_YKB_BACKLIGHT

static void vendor_hid_protocol_script_slot_to_wire(
    vendor_hid_proto_script_slot_payload_t *out,
    const ykb_backlight_script_slot_t *in) {
    out->occupied = in->occupied ? 1U : 0U;
    out->size = in->size;
    memcpy(out->name, in->name, sizeof(out->name));
    memcpy(out->bytecode, in->bytecode, sizeof(out->bytecode));
}

static void vendor_hid_protocol_script_slot_from_wire(
    ykb_backlight_script_slot_t *out,
    const vendor_hid_proto_script_slot_payload_t *in) {
    out->occupied = in->occupied != 0U;
    out->size = in->size;
    memcpy(out->name, in->name, sizeof(out->name));
    out->name[ARRAY_SIZE(out->name) - 1] = '\0';
    memcpy(out->bytecode, in->bytecode, sizeof(out->bytecode));
}

static int vendor_hid_protocol_get_script_slot(
    uint16_t slot, vendor_hid_proto_script_slot_packet_t *out) {
    ykb_backlight_script_slot_t slot_data;
    int err;

    err = ykb_backlight_get_script_slot(slot, &slot_data);
    if (err) {
        LOG_ERR("ykb_backlight_get_script_slot(%u): %d", slot, err);
        return err;
    }

    out->slot = slot;
    vendor_hid_protocol_script_slot_to_wire(&out->payload, &slot_data);

    return 0;
}

static int vendor_hid_protocol_set_script_slot(
    const vendor_hid_proto_script_slot_packet_t *in) {
    ykb_backlight_script_slot_t slot_data;
    int err;

    vendor_hid_protocol_script_slot_from_wire(&slot_data, &in->payload);
    err = ykb_backlight_set_script_slot(in->slot, &slot_data);
    if (err) {
        LOG_ERR("ykb_backlight_set_script_slot(%u): %d", in->slot, err);
    }

    return err;
}

static int vendor_hid_protocol_get_script_slot_info(
    uint16_t slot, vendor_hid_proto_script_slot_info_t *out) {
    ykb_backlight_script_slot_t slot_data;
    int err;

    err = ykb_backlight_get_script_slot(slot, &slot_data);
    if (err) {
        LOG_ERR("ykb_backlight_get_script_slot(%u): %d", slot, err);
        return err;
    }

    out->slot = slot;
    out->occupied = slot_data.occupied ? 1U : 0U;
    out->size = slot_data.size;
    memcpy(out->name, slot_data.name, sizeof(out->name));

    return 0;
}

static int vendor_hid_protocol_clear_script_slot(uint16_t slot) {
    ykb_backlight_script_slot_t slot_data = {0};
    int err = ykb_backlight_set_script_slot(slot, &slot_data);
    if (err) {
        LOG_ERR("ykb_backlight_set_script_slot(clear %u): %d", slot, err);
    }

    return err;
}

static int vendor_hid_protocol_rename_script_slot(
    const vendor_hid_proto_script_slot_rename_request_t *in) {
    ykb_backlight_script_slot_t slot_data;
    int err;

    err = ykb_backlight_get_script_slot(in->slot, &slot_data);
    if (err) {
        LOG_ERR("ykb_backlight_get_script_slot(%u): %d", in->slot, err);
        return err;
    }

    memcpy(slot_data.name, in->name, sizeof(slot_data.name));
    slot_data.name[ARRAY_SIZE(slot_data.name) - 1] = '\0';

    err = ykb_backlight_set_script_slot(in->slot, &slot_data);
    if (err) {
        LOG_ERR("ykb_backlight_set_script_slot(rename %u): %d", in->slot, err);
    }

    return err;
}

#endif // CONFIG_YKB_BACKLIGHT

#if CONFIG_YKB_BATTSENSE

static inline int
vendor_hid_protocol_get_battery_state(ykb_battsense_state_t *state) {
    return ykb_battsense_get_state(state);
}

#endif // CONFIG_YKB_BATTSENSE

#if CONFIG_SPLITLINK_SYNC_MASTER

static void on_slave_battery_state(ykb_battsense_state_t *state) {
    k_mutex_lock(&secondary_batt_state_mut, K_FOREVER);
    memcpy(&secondary_batt_state, state, sizeof(ykb_battsense_state_t));
    k_mutex_unlock(&secondary_batt_state_mut);
}

SPLITLINK_SYNC_CB(vendor_hid) = {
    .on_slave_battery_state = on_slave_battery_state,
};

#endif // CONFIG_SPLITLINK_SYNC_MASTER

static void response_work_handler(struct k_work *work) {
    vendor_hid_protocol_ctx_t *ctx =
        CONTAINER_OF(work, vendor_hid_protocol_ctx_t, response_work);
    uint16_t values[TOTAL_KEY_COUNT];
    uint8_t response_code;
    uint8_t *data;
    uint16_t len;

    switch (ctx->current_response) {
    case RESPONSE_GET_FEATURES: {
        data = (uint8_t *)&features;
        len = sizeof(features);
        break;
    }
    case RESPONSE_GET_VALUES: {
        kb_handler_get_raw_values(values, TOTAL_KEY_COUNT);
        data = (uint8_t *)values;
        len = sizeof(values);
        break;
    }
    case RESPONSE_GET_SETTINGS: {
        kb_settings_t *settings = vendor_hid_protocol_get_settings();
        if (!settings) {
            response_code = RESPONSE_ERROR;
            data = &response_code;
            len = sizeof(response_code);
            break;
        }
        data = (uint8_t *)settings;
        len = sizeof(*settings);
        break;
    }
    case RESPONSE_SET_SETTINGS_OK: {
        response_code = RESPONSE_SET_SETTINGS_OK;
        data = &response_code;
        len = sizeof(response_code);
        break;
    }
#if CONFIG_YKB_BACKLIGHT
    case RESPONSE_GET_LUMISCRIPT_SLOT: {
        const vendor_hid_proto_packet_t *request =
            (const vendor_hid_proto_packet_t *)ctx->rx_buffer;
        const vendor_hid_proto_script_slot_get_request_t *slot_request =
            (const vendor_hid_proto_script_slot_get_request_t *)request->data;
        int err = vendor_hid_protocol_get_script_slot(slot_request->slot,
                                                      &script_slot_snap);
        if (err) {
            response_code = RESPONSE_ERROR;
            data = &response_code;
            len = sizeof(response_code);
            break;
        }
        data = (uint8_t *)&script_slot_snap;
        len = sizeof(script_slot_snap);
        break;
    }
    case RESPONSE_SET_LUMISCRIPT_SLOT_OK: {
        response_code = RESPONSE_SET_LUMISCRIPT_SLOT_OK;
        data = &response_code;
        len = sizeof(response_code);
        break;
    }
    case RESPONSE_GET_LUMISCRIPT_SLOT_INFO: {
        const vendor_hid_proto_packet_t *request =
            (const vendor_hid_proto_packet_t *)ctx->rx_buffer;
        const vendor_hid_proto_script_slot_get_request_t *slot_request =
            (const vendor_hid_proto_script_slot_get_request_t *)request->data;
        int err = vendor_hid_protocol_get_script_slot_info(
            slot_request->slot, &script_slot_info_snap);
        if (err) {
            response_code = RESPONSE_ERROR;
            data = &response_code;
            len = sizeof(response_code);
            break;
        }
        data = (uint8_t *)&script_slot_info_snap;
        len = sizeof(script_slot_info_snap);
        break;
    }
    case RESPONSE_CLEAR_LUMISCRIPT_SLOT_OK: {
        response_code = RESPONSE_CLEAR_LUMISCRIPT_SLOT_OK;
        data = &response_code;
        len = sizeof(response_code);
        break;
    }
    case RESPONSE_RENAME_LUMISCRIPT_SLOT_OK: {
        response_code = RESPONSE_RENAME_LUMISCRIPT_SLOT_OK;
        data = &response_code;
        len = sizeof(response_code);
        break;
    }
#endif // CONFIG_YKB_BACKLIGHT
#if CONFIG_YKB_BATTSENSE
    case RESPONSE_GET_BATTERY_STATE: {
        int err = vendor_hid_protocol_get_battery_state(&batt_state);
        if (err) {
            response_code = RESPONSE_ERROR;
            data = &response_code;
            len = sizeof(response_code);
            break;
        }
        response_code = RESPONSE_GET_BATTERY_STATE;
        data = (uint8_t *)&batt_state;
        len = sizeof(batt_state);
        break;
    }
#endif // CONFIG_YKB_BATTSENSE
#if CONFIG_SPLITLINK_SYNC_MASTER
    case RESPONSE_GET_SECONDARY_BATTERY_STATE: {
        static ykb_battsense_state_t secondary_batt_state_snap;

        k_mutex_lock(&secondary_batt_state_mut, K_FOREVER);
        memcpy(&secondary_batt_state_snap, &secondary_batt_state,
               sizeof(secondary_batt_state_snap));
        k_mutex_unlock(&secondary_batt_state_mut);

        response_code = RESPONSE_GET_SECONDARY_BATTERY_STATE;
        data = (uint8_t *)&secondary_batt_state_snap;
        len = sizeof(secondary_batt_state_snap);
        break;
    }
#endif // CONFIG_SPLITLINK_SYNC_MASTER
    case RESPONSE_ERROR:
    default: {
        response_code = RESPONSE_ERROR;
        data = &response_code;
        len = sizeof(response_code);
        break;
    }
    }

    ykb_protocol_tx_state_t tx;
    ykb_protocol_tx_init(&tx, data, len, 0, YKB_PROTOCOL_TYPE_DATA);

    ykb_protocol_packet_t packet;
    while (ykb_protocol_tx_has_more(&tx)) {
        if (!ykb_protocol_tx_build_packet(&tx, &packet)) {
            LOG_ERR("ykb_protocol_tx_build_packet failed");
            break;
        }

        uint16_t payload_len = ykb_protocol_payload_len_for_index(
            tx.total_len, packet.header.packet_idx, tx.packet_count);
        size_t packet_len = sizeof(packet.header) + payload_len;

        int err = ctx->send_packet((const uint8_t *)&packet, packet_len,
                                   ctx->user_data);
        if (err) {
            LOG_ERR("send_packet failed: %d", err);
            break;
        }
    }

    ykb_protocol_rx_reset(&ctx->rx);
    ctx->busy = false;
}

int vendor_hid_protocol_init(vendor_hid_protocol_ctx_t *ctx,
                             vendor_hid_send_packet_cb_t send_packet,
                             void *user_data) {
    if (!ctx || !send_packet) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->send_packet = send_packet;
    ctx->user_data = user_data;

    ykb_protocol_rx_init(&ctx->rx, ctx->rx_buffer, sizeof(ctx->rx_buffer),
                         false, NULL, 0);
    k_work_init(&ctx->response_work, response_work_handler);

    return 0;
}

int vendor_hid_protocol_parse(vendor_hid_protocol_ctx_t *ctx,
                              const uint8_t *data, size_t len) {
    if (!ctx || !data) {
        return -EINVAL;
    }

    if (ctx->busy) {
        LOG_WRN("Response in progress, skipping packet");
        return -EBUSY;
    }

    if (len < sizeof(ykb_protocol_header_t)) {
        LOG_ERR("Packet too short: %u", (unsigned)len);
        return -EMSGSIZE;
    }

    ykb_protocol_rx_result_t res = ykb_protocol_rx_push_packet(
        &ctx->rx, (const ykb_protocol_packet_t *)data);
    if (res < 0) {
        LOG_ERR("ykb_protocol_rx_push_packet: %d", res);
        return (int)res;
    }

    if (res != YKB_PROTOCOL_RX_RESULT_COMPLETE) {
        return 0;
    }

    ctx->busy = true;

    const vendor_hid_proto_packet_t *request =
        (const vendor_hid_proto_packet_t *)ctx->rx_buffer;

    switch ((enum request_type)request->header.type) {
    case REQUEST_GET_FEATURES: {
        ctx->current_response = RESPONSE_GET_FEATURES;
        break;
    }
    case REQUEST_GET_VALUES: {
        ctx->current_response = RESPONSE_GET_VALUES;
        break;
    }
    case REQUEST_GET_SETTINGS: {
        ctx->current_response = RESPONSE_GET_SETTINGS;
        break;
    }
    case REQUEST_SET_SETTINGS: {
        int err = vendor_hid_protocol_set_settings(
            (const kb_settings_t *)request->data);
        ctx->current_response =
            (err == 0) ? RESPONSE_SET_SETTINGS_OK : RESPONSE_ERROR;
        break;
    }
#if CONFIG_YKB_BACKLIGHT
    case REQUEST_GET_LUMISCRIPT_SLOT: {
        ctx->current_response = RESPONSE_GET_LUMISCRIPT_SLOT;
        break;
    }
    case REQUEST_SET_LUMISCRIPT_SLOT: {
        int err = vendor_hid_protocol_set_script_slot(
            (const vendor_hid_proto_script_slot_packet_t *)request->data);
        ctx->current_response =
            (err == 0) ? RESPONSE_SET_LUMISCRIPT_SLOT_OK : RESPONSE_ERROR;
        break;
    }
    case REQUEST_GET_LUMISCRIPT_SLOT_INFO: {
        ctx->current_response = RESPONSE_GET_LUMISCRIPT_SLOT_INFO;
        break;
    }
    case REQUEST_CLEAR_LUMISCRIPT_SLOT: {
        const vendor_hid_proto_script_slot_get_request_t *slot_request =
            (const vendor_hid_proto_script_slot_get_request_t *)request->data;
        int err = vendor_hid_protocol_clear_script_slot(slot_request->slot);
        ctx->current_response =
            (err == 0) ? RESPONSE_CLEAR_LUMISCRIPT_SLOT_OK : RESPONSE_ERROR;
        break;
    }
    case REQUEST_RENAME_LUMISCRIPT_SLOT: {
        int err = vendor_hid_protocol_rename_script_slot(
            (const vendor_hid_proto_script_slot_rename_request_t *)
                request->data);
        ctx->current_response =
            (err == 0) ? RESPONSE_RENAME_LUMISCRIPT_SLOT_OK : RESPONSE_ERROR;
        break;
    }
#endif // CONFIG_YKB_BACKLIGHT
#if CONFIG_YKB_BATTSENSE
    case REQUEST_GET_BATTERY_STATE: {
        ctx->current_response = RESPONSE_GET_BATTERY_STATE;
        break;
    }
#endif // CONFIG_YKB_BATTSENSE
#if CONFIG_SPLITLINK_SYNC_MASTER
    case REQUEST_GET_SECONDARY_BATTERY_STATE: {
        ctx->current_response = RESPONSE_GET_SECONDARY_BATTERY_STATE;
        break;
    }
#endif // CONFIG_SPLITLINK_SYNC_MASTER
    default: {
        LOG_ERR("Unknown request type %u", request->header.type);
        ctx->current_response = RESPONSE_ERROR;
        break;
    }
    }

    k_work_submit(&ctx->response_work);

    return 0;
}
