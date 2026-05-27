#include "hid_devices.h"

#include <subsys/kb_handler.h>

#include <zephyr/usb/class/hid.h>

#define BT_CONNECT_MOUSE_INPUT_REPORT_ID 2U

static uint8_t input_report_index;

static const uint8_t hid_mouse_report_desc[] = {
    HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_MOUSE),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
    HID_REPORT_ID(BT_CONNECT_MOUSE_INPUT_REPORT_ID),

    HID_USAGE(HID_USAGE_GEN_DESKTOP_POINTER),
    HID_COLLECTION(HID_COLLECTION_PHYSICAL),

    HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
    HID_USAGE_MIN8(1),
    HID_USAGE_MAX8(3),
    HID_LOGICAL_MIN8(0),
    HID_LOGICAL_MAX8(1),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(3),
    HID_INPUT(0x02),

    HID_REPORT_SIZE(5),
    HID_REPORT_COUNT(1),
    HID_INPUT(0x01),

    HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_X),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_Y),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_WHEEL),
    HID_LOGICAL_MIN8(-127),
    HID_LOGICAL_MAX8(127),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(3),
    HID_INPUT(0x06),

    HID_END_COLLECTION,
    HID_END_COLLECTION,
};

static int append_mouse_hids_init(struct bt_hids_init_param *init,
                                  uint8_t *input_count, uint8_t *output_count,
                                  uint8_t *feature_count) {
    input_report_index = *input_count;

    struct bt_hids_inp_rep *mouse_inp =
        &init->inp_rep_group_init.reports[*input_count];

    mouse_inp->size = sizeof(hid_mouse_report_t);
    mouse_inp->id = BT_CONNECT_MOUSE_INPUT_REPORT_ID;
    (*input_count)++;

    init->is_mouse = true;

    return 0;
}

static int send_mouse_report_cb(const struct bt_connect_conn_state *state,
                                void *user_data) {
    const hid_mouse_report_t *report = user_data;

    if (state->in_boot_mode) {
        return bt_hids_boot_mouse_inp_rep_send(bt_connect_hids_obj(),
                                               state->conn, &report->buttons,
                                               report->x, report->y, NULL);
    }

    return bt_hids_inp_rep_send(bt_connect_hids_obj(), state->conn,
                                input_report_index, (const uint8_t *)report,
                                sizeof(*report), NULL);
}

int bt_connect_mouse_send_report(const hid_mouse_report_t *report) {
    if (!report) {
        return -EINVAL;
    }

    bt_connect_foreach_conn(send_mouse_report_cb, (void *)report);
    return 0;
}

BT_CONNECT_REGISTER_HID_REPORT(bt_connect_mouse_hid, hid_mouse_report_desc,
                               append_mouse_hids_init);
