#include <drivers/splitlink_bt.h>
#include <subsys/bt_connect.h>

#include "hid_devices/hid_devices.h"

#include <subsys/ykb_battsense.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bt_connect, CONFIG_BT_CONNECT_LOG_LEVEL);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION 0x0101

#define BT_CONNECT_KBD_INPUT_REPORT_SIZE sizeof(hid_kb_report_t)
#define BT_CONNECT_KBD_OUTPUT_REPORT_SIZE 1U
#define BT_CONNECT_MOUSE_INPUT_REPORT_SIZE sizeof(hid_mouse_report_t)
#define BT_CONNECT_VENDOR_INPUT_REPORT_SIZE                                    \
    CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE
#define BT_CONNECT_VENDOR_OUTPUT_REPORT_SIZE                                   \
    CONFIG_BT_CONNECT_MAX_VENDOR_OUT_REPORT_SIZE
#define BT_CONNECT_VENDOR_FEATURE_REPORT_SIZE                                  \
    CONFIG_BT_CONNECT_MAX_VENDOR_IN_REPORT_SIZE

BT_HIDS_DEF(hids_obj,
#if CONFIG_BT_CONNECT_KBD
            BT_CONNECT_KBD_INPUT_REPORT_SIZE, BT_CONNECT_KBD_OUTPUT_REPORT_SIZE,
#endif
#if CONFIG_BT_CONNECT_MOUSE
            BT_CONNECT_MOUSE_INPUT_REPORT_SIZE,
#endif
#if CONFIG_BT_CONNECT_VENDOR
            BT_CONNECT_VENDOR_OUTPUT_REPORT_SIZE,
            BT_CONNECT_VENDOR_INPUT_REPORT_SIZE,
            BT_CONNECT_VENDOR_FEATURE_REPORT_SIZE,
#endif
);

static struct bt_connect_conn_state
    conn_states[CONFIG_BT_HIDS_MAX_CLIENT_COUNT];

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
                  (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
                  (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static bool is_advertising;
static uint8_t assembled_report_map[CONFIG_BT_CONNECT_REPORT_MAP_MAX_SIZE];
static size_t assembled_report_map_size;
static bool battery_notifications_ready;
static bool battery_level_pending_valid;
static uint8_t pending_battery_level;

static void publish_pending_battery_level(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_level_publish_work,
                               publish_pending_battery_level);

#if CONFIG_BT_CONNECT_KBD
int bt_connect_keyboard_send_report(const hid_kb_report_t *report);
#endif
#if CONFIG_BT_CONNECT_MOUSE
int bt_connect_mouse_send_report(const hid_mouse_report_t *report);
#endif

struct bt_hids *bt_connect_hids_obj(void) { return &hids_obj; }

static void publish_pending_battery_level(struct k_work *work) {
    ARG_UNUSED(work);

    if (!battery_notifications_ready || !battery_level_pending_valid) {
        return;
    }

    int err = bt_bas_set_battery_level(MIN(pending_battery_level, 100U));
    if (err) {
        LOG_ERR("Failed to update BAS battery level (%d)", err);
        return;
    }

    battery_level_pending_valid = false;
}

void bt_connect_foreach_conn(bt_connect_conn_iter_fn_t fn, void *user_data) {
    if (!fn) {
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(conn_states); ++i) {
        if (!conn_states[i].conn) {
            continue;
        }

        int err = fn(&conn_states[i], user_data);
        if (err) {
            if (err == -EACCES) {
                LOG_WRN("BLE HID report skipped: peer not subscribed yet");
            } else if (err == -ENOTCONN) {
                LOG_WRN("BLE HID report skipped: peer disconnected");
            } else {
                LOG_ERR("BLE HID connection callback failed (%d)", err);
            }
        }
    }
}

static bool any_connected(void) {
    for (size_t i = 0; i < ARRAY_SIZE(conn_states); ++i) {
        if (conn_states[i].conn != NULL) {
            return true;
        }
    }

    return false;
}

bool bt_connect_can_send_kb_report(void) { return any_connected(); }

bool bt_connect_can_send_mouse_report(void) { return any_connected(); }

static void notify_connected(const bt_addr_le_t *addr) {
    STRUCT_SECTION_FOREACH(bt_connect_cb, cb) {
        if (cb->on_connect) {
            cb->on_connect(addr);
        }
    }
}

static void notify_disconnected(const bt_addr_le_t *addr) {
    STRUCT_SECTION_FOREACH(bt_connect_cb, cb) {
        if (cb->on_disconnect) {
            cb->on_disconnect(addr);
        }
    }
}

static void advertising_start(void) {
    int err;
    const struct bt_le_adv_param *adv_param =
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
                        BT_GAP_ADV_FAST_INT_MAX_2, NULL);

    err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err && err != -EALREADY) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return;
    }

    is_advertising = true;
    LOG_INF("Advertising started");
}

static void maybe_restart_advertising(void) {
    if (!any_connected() && !is_advertising) {
        advertising_start();
    }
}

static struct bt_connect_conn_state *find_conn_state(struct bt_conn *conn) {
    for (size_t i = 0; i < ARRAY_SIZE(conn_states); ++i) {
        if (conn_states[i].conn == conn) {
            return &conn_states[i];
        }
    }

    return NULL;
}

static struct bt_connect_conn_state *alloc_conn_state(void) {
    for (size_t i = 0; i < ARRAY_SIZE(conn_states); ++i) {
        if (conn_states[i].conn == NULL) {
            return &conn_states[i];
        }
    }

    return NULL;
}

static bool bt_connect_manages_conn(struct bt_conn *conn) {
    struct bt_conn_info info;
    int err = bt_conn_get_info(conn, &info);

    if (err) {
        LOG_WRN("Failed to get Bluetooth connection info (%d)", err);
        return false;
    }

    return info.role == BT_CONN_ROLE_PERIPHERAL;
}

static void hids_pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn) {
    struct bt_connect_conn_state *state = find_conn_state(conn);

    if (!state) {
        LOG_WRN("HIDS protocol mode event for unknown connection");
        return;
    }

    switch (evt) {
    case BT_HIDS_PM_EVT_BOOT_MODE_ENTERED:
        state->in_boot_mode = true;
        break;

    case BT_HIDS_PM_EVT_REPORT_MODE_ENTERED:
        state->in_boot_mode = false;
        break;

    default:
        break;
    }
}

static void connected(struct bt_conn *conn, uint8_t err) {
    struct bt_connect_conn_state *state;
    bt_addr_le_t addr;
    int sec_err;

    bt_addr_le_copy(&addr, bt_conn_get_dst(conn));

    if (err) {
        LOG_ERR("Failed to connect (%d)", err);
        return;
    }

    if (!bt_connect_manages_conn(conn)) {
        return;
    }

    state = alloc_conn_state();
    if (!state) {
        LOG_ERR("No free Bluetooth connection slots");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    err = bt_hids_connected(&hids_obj, conn);
    if (err) {
        LOG_ERR("Failed to notify HIDS about connection (%d)", err);
        return;
    }

    state->conn = bt_conn_ref(conn);
    state->in_boot_mode = false;
    is_advertising = false;
    battery_notifications_ready = false;

    sec_err = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (sec_err && sec_err != -EALREADY) {
        LOG_WRN("Failed to request Bluetooth security level 2 (%d)", sec_err);
    }

    notify_connected(&addr);
    LOG_INF("Bluetooth connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    struct bt_connect_conn_state *state = find_conn_state(conn);
    bt_addr_le_t addr;
    int err;

    if (!bt_connect_manages_conn(conn) && !state) {
        return;
    }

    bt_addr_le_copy(&addr, bt_conn_get_dst(conn));

    err = bt_hids_disconnected(&hids_obj, conn);
    if (err) {
        LOG_ERR("Failed to notify HIDS about disconnection (%d)", err);
    }

    if (state) {
        bt_conn_unref(state->conn);
        state->conn = NULL;
        state->in_boot_mode = false;
    }

    battery_notifications_ready = false;
    (void)k_work_cancel_delayable(&battery_level_publish_work);

    notify_disconnected(&addr);
    LOG_INF("Bluetooth disconnected (reason 0x%02x)", reason);

    maybe_restart_advertising();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err) {
    if (!bt_connect_manages_conn(conn)) {
        return;
    }

    if (err) {
        LOG_WRN("Security failed, level %u err %d", level, err);
        return;
    }

    LOG_INF("Security changed, level %u", level);

    if (level >= BT_SECURITY_L2) {
        battery_notifications_ready = true;
        k_work_reschedule(&battery_level_publish_work, K_MSEC(500));
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static void auth_cancel(struct bt_conn *conn) {
    char addr[BT_ADDR_LE_STR_LEN];

    if (!bt_connect_manages_conn(conn)) {
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded) {
    char addr[BT_ADDR_LE_STR_LEN];

    if (!bt_connect_manages_conn(conn)) {
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    if (!bt_connect_manages_conn(conn)) {
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("Pairing failed: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

void bt_connect_set_battery_level(uint8_t percentage) {
    pending_battery_level = MIN(percentage, 100U);
    battery_level_pending_valid = true;

    if (battery_notifications_ready) {
        int err = k_work_reschedule(&battery_level_publish_work, K_MSEC(50));
        if (err < 0) {
            LOG_ERR("Failed to schedule BAS update (%d)", err);
        }
    }
}

void bt_connect_send_kb_report(const hid_kb_report_t *report) {
#if CONFIG_BT_CONNECT_KBD
    int err = bt_connect_keyboard_send_report(report);
    if (err) {
        LOG_ERR("Failed to send keyboard report over BLE (%d)", err);
    }
#else
    ARG_UNUSED(report);
#endif
}

void bt_connect_send_mouse_report(const hid_mouse_report_t *report) {
#if CONFIG_BT_CONNECT_MOUSE
    int err = bt_connect_mouse_send_report(report);
    if (err) {
        LOG_ERR("Failed to send mouse report over BLE (%d)", err);
    }
#else
    ARG_UNUSED(report);
#endif
}

static int assemble_report_map(void) {
    assembled_report_map_size = 0;

    STRUCT_SECTION_FOREACH(bt_connect_hid_report, report) {
        if (!report->report_map || report->report_map_size == 0) {
            continue;
        }

        if (assembled_report_map_size + report->report_map_size >
            sizeof(assembled_report_map)) {
            LOG_ERR("BLE HID report map too large after '%s'",
                    report->name ? report->name : "unknown");
            return -ENOMEM;
        }

        memcpy(&assembled_report_map[assembled_report_map_size],
               report->report_map, report->report_map_size);
        assembled_report_map_size += report->report_map_size;
    }

    return 0;
}

static int bt_connect_hid_init(void) {
    struct bt_hids_init_param init = {0};
    uint8_t input_count = 0;
    uint8_t output_count = 0;
    uint8_t feature_count = 0;
    int err;

    err = assemble_report_map();
    if (err) {
        return err;
    }

    init.rep_map.data = assembled_report_map;
    init.rep_map.size = assembled_report_map_size;

    init.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
    init.info.b_country_code = 0x00;
    init.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;
    init.pm_evt_handler = hids_pm_evt_handler;

    STRUCT_SECTION_FOREACH(bt_connect_hid_report, report) {
        if (!report->append_init) {
            continue;
        }

        err = report->append_init(&init, &input_count, &output_count,
                                  &feature_count);
        if (err) {
            LOG_ERR("BLE HID contributor '%s' init failed (%d)",
                    report->name ? report->name : "unknown", err);
            return err;
        }
    }

    init.inp_rep_group_init.cnt = input_count;
    init.outp_rep_group_init.cnt = output_count;
    init.feat_rep_group_init.cnt = feature_count;

    err = bt_hids_init(&hids_obj, &init);
    if (err) {
        LOG_ERR("HIDS initialization failed (%d)", err);
        return err;
    }

    return 0;
}

#if CONFIG_YKB_BATTSENSE
static void on_battery_state_changed(ykb_battsense_state_t state) {
    bt_connect_set_battery_level(state.percentage);
}

static YKB_BATTSENSE_DEFINE(bt_connect_battery_transport) = {
    .on_state_changed = on_battery_state_changed,
    .on_low_percentage = on_battery_state_changed,
    .on_critical_percentage = on_battery_state_changed,
};
#endif

static int bt_connect_init(void) {
    int err;

    err = bt_conn_auth_cb_register(&conn_auth_callbacks);
    if (err) {
        LOG_ERR("Failed to register authorization callbacks (%d)", err);
        return err;
    }

    err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
    if (err) {
        LOG_ERR("Failed to register authorization info callbacks (%d)", err);
        return err;
    }

    err = bt_connect_hid_init();
    if (err) {
        return err;
    }

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return err;
    }

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

#if CONFIG_YKB_BATTSENSE
    ykb_battsense_state_t state;

    err = ykb_battsense_get_state(&state);
    if (!err) {
        bt_connect_set_battery_level(state.percentage);
    }
#endif

    advertising_start();

#if CONFIG_SPLITLINK_BT_CENTRAL
    err = splitlink_bt_start();
    if (err && err != -EALREADY) {
        LOG_ERR("Failed to start Splitlink BT central (%d)", err);
        return err;
    }
#endif // CONFIG_SPLITLINK_BT_CENTRAL

    return 0;
}

SYS_INIT(bt_connect_init, POST_KERNEL, CONFIG_BT_CONNECT_INIT_PRIORITY);
