#include "hid_devices.h"

#include <subsys/vendor_hid_protocol.h>

#include <subsys/bt_connect.h>

#include <zephyr/logging/log.h>
#include <zephyr/usb/class/hid.h>

LOG_MODULE_DECLARE(bt_connect, CONFIG_BT_CONNECT_LOG_LEVEL);

#define BT_CONNECT_VENDOR_INPUT_REPORT_ID 3U
#define BT_CONNECT_VENDOR_OUTPUT_REPORT_ID 4U
#define BT_CONNECT_VENDOR_FEATURE_REPORT_ID 5U

static uint8_t input_report_index;
static vendor_hid_protocol_ctx_t vendor_protocol_ctx;

static const uint8_t hid_vendor_report_desc[] = {
    HID_USAGE_PAGE(0xFF),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),

    HID_REPORT_ID(BT_CONNECT_VENDOR_INPUT_REPORT_ID),
    HID_USAGE(0x02),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX8(0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE),
    HID_INPUT(0x02),

    HID_REPORT_ID(BT_CONNECT_VENDOR_OUTPUT_REPORT_ID),
    HID_USAGE(0x03),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX8(0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(CONFIG_BT_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE),
    HID_OUTPUT(0x02),

    HID_REPORT_ID(BT_CONNECT_VENDOR_FEATURE_REPORT_ID),
    HID_USAGE(0x04),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX8(0xFF),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE),
    HID_FEATURE(0x02),

    HID_END_COLLECTION,
};

static int send_vendor_packet_cb(const struct bt_connect_conn_state *state,
                                 void *user_data) {
    struct {
        const uint8_t *data;
        size_t len;
    } *packet = user_data;

    return bt_hids_inp_rep_send(bt_connect_hids_obj(), state->conn,
                                input_report_index, packet->data, packet->len,
                                NULL);
}

static int send_vendor_packet(const uint8_t *data, size_t len,
                              void *user_data) {
    struct {
        const uint8_t *data;
        size_t len;
    } packet = {
        .data = data,
        .len = len,
    };

    bt_connect_foreach_conn(send_vendor_packet_cb, &packet);
    return 0;
}

static void hids_vendor_outp_rep_handler(struct bt_hids_rep *rep,
                                         struct bt_conn *conn, bool write) {
    if (!write) {
        return;
    }

    int err =
        vendor_hid_protocol_parse(&vendor_protocol_ctx, rep->data, rep->size);
    if (err) {
        LOG_ERR("vendor_hid_protocol_parse: %d", err);
    }
}

static int append_vendor_hids_init(struct bt_hids_init_param *init,
                                   uint8_t *input_count, uint8_t *output_count,
                                   uint8_t *feature_count) {
    int err = vendor_hid_protocol_init(&vendor_protocol_ctx, send_vendor_packet,
                                       NULL);
    if (err) {
        return err;
    }

    input_report_index = *input_count;

    struct bt_hids_inp_rep *vendor_in =
        &init->inp_rep_group_init.reports[*input_count];
    struct bt_hids_outp_feat_rep *vendor_out =
        &init->outp_rep_group_init.reports[*output_count];
    struct bt_hids_outp_feat_rep *vendor_feat =
        &init->feat_rep_group_init.reports[*feature_count];

    vendor_in->size = CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE;
    vendor_in->id = BT_CONNECT_VENDOR_INPUT_REPORT_ID;
    (*input_count)++;

    vendor_out->size = CONFIG_BT_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE;
    vendor_out->id = BT_CONNECT_VENDOR_OUTPUT_REPORT_ID;
    vendor_out->handler = hids_vendor_outp_rep_handler;
    (*output_count)++;

    vendor_feat->size = CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE;
    vendor_feat->id = BT_CONNECT_VENDOR_FEATURE_REPORT_ID;
    (*feature_count)++;

    return 0;
}

BT_CONNECT_REGISTER_HID_REPORT(bt_connect_vendor_hid, hid_vendor_report_desc,
                               append_vendor_hids_init);
