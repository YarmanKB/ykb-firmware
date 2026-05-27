#include "hid_devices.h"

#include <subsys/bt_connect.h>
#include <subsys/kb_handler.h>

#include <zephyr/logging/log.h>
#include <zephyr/usb/class/hid.h>

LOG_MODULE_DECLARE(bt_connect, CONFIG_BT_CONNECT_LOG_LEVEL);

#if CONFIG_BT_CONNECT_MOUSE || CONFIG_BT_CONNECT_VENDOR
#define BT_CONNECT_KBD_INPUT_REPORT_ID 1U
#define BT_CONNECT_KBD_OUTPUT_REPORT_ID 6U
#else
#define BT_CONNECT_KBD_INPUT_REPORT_ID 0U
#define BT_CONNECT_KBD_OUTPUT_REPORT_ID 0U
#endif

static uint8_t input_report_index;

static const uint8_t hid_kbd_report_desc[] = {
    HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_KEYBOARD),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
#if BT_CONNECT_KBD_INPUT_REPORT_ID
    HID_REPORT_ID(BT_CONNECT_KBD_INPUT_REPORT_ID),
#endif

    HID_USAGE_PAGE(HID_USAGE_GEN_KEYBOARD),
    HID_USAGE_MIN8(0xE0),
    HID_USAGE_MAX8(0xE7),
    HID_LOGICAL_MIN8(0),
    HID_LOGICAL_MAX8(1),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(8),
    HID_INPUT(0x02),

    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(1),
    HID_INPUT(0x03),

    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(6),
    HID_LOGICAL_MIN8(0),
    HID_LOGICAL_MAX8(101),
    HID_USAGE_PAGE(HID_USAGE_GEN_KEYBOARD),
    HID_USAGE_MIN8(0),
    HID_USAGE_MAX8(101),
    HID_INPUT(0x00),

#if BT_CONNECT_KBD_OUTPUT_REPORT_ID
    HID_REPORT_ID(BT_CONNECT_KBD_OUTPUT_REPORT_ID),
#endif
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(5),
    HID_USAGE_PAGE(HID_USAGE_GEN_LEDS),
    HID_USAGE_MIN8(1),
    HID_USAGE_MAX8(5),
    HID_OUTPUT(0x02),

    HID_REPORT_SIZE(3),
    HID_REPORT_COUNT(1),
    HID_OUTPUT(0x03),

    HID_END_COLLECTION,
};

static void caps_lock_handler(const struct bt_hids_rep *rep) {
    uint8_t caps_on = ((*rep->data) & 0x02U) ? 1U : 0U;
    LOG_INF("CAPS lock %s", caps_on ? "on" : "off");
}

static void hids_outp_rep_handler(struct bt_hids_rep *rep, struct bt_conn *conn,
                                  bool write) {
    if (write) {
        caps_lock_handler(rep);
    }
}

static void hids_boot_kb_outp_rep_handler(struct bt_hids_rep *rep,
                                          struct bt_conn *conn, bool write) {
    if (write) {
        caps_lock_handler(rep);
    }
}

static int append_kbd_hids_init(struct bt_hids_init_param *init,
                                uint8_t *input_count, uint8_t *output_count,
                                uint8_t *feature_count) {
    input_report_index = *input_count;

    struct bt_hids_inp_rep *kbd_inp =
        &init->inp_rep_group_init.reports[*input_count];
    struct bt_hids_outp_feat_rep *kbd_out =
        &init->outp_rep_group_init.reports[*output_count];

    kbd_inp->size = sizeof(hid_kb_report_t);
    kbd_inp->id = BT_CONNECT_KBD_INPUT_REPORT_ID;
    (*input_count)++;

    kbd_out->size = 1U;
    kbd_out->id = BT_CONNECT_KBD_OUTPUT_REPORT_ID;
    kbd_out->handler = hids_outp_rep_handler;
    (*output_count)++;

    init->is_kb = true;
    init->boot_kb_outp_rep_handler = hids_boot_kb_outp_rep_handler;

    return 0;
}

static int send_kb_report_cb(const struct bt_connect_conn_state *state,
                             void *user_data) {
    const hid_kb_report_t *report = user_data;

    if (state->in_boot_mode) {
        return bt_hids_boot_kb_inp_rep_send(bt_connect_hids_obj(), state->conn,
                                            (const uint8_t *)report,
                                            sizeof(*report), NULL);
    }

    return bt_hids_inp_rep_send(bt_connect_hids_obj(), state->conn,
                                input_report_index, (const uint8_t *)report,
                                sizeof(*report), NULL);
}

int bt_connect_keyboard_send_report(const hid_kb_report_t *report) {
    if (!report) {
        return -EINVAL;
    }

    bt_connect_foreach_conn(send_kb_report_cb, (void *)report);
    return 0;
}

BT_CONNECT_REGISTER_HID_REPORT(bt_connect_kbd_hid, hid_kbd_report_desc,
                               append_kbd_hids_init);
