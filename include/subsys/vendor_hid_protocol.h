#ifndef SUBSYS_VENDOR_HID_PROTOCOL_H
#define SUBSYS_VENDOR_HID_PROTOCOL_H

#include <lib/features.h>
#include <lib/ykb_protocol.h>

#include <subsys/kb_settings.h>
#include <subsys/ykb_backlight.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util_macro.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum request_type {
    REQUEST_GET_FEATURES = 0U,
    REQUEST_GET_VALUES = 1U,
    REQUEST_GET_SETTINGS = 2U,
    REQUEST_SET_SETTINGS = 3U,

#if CONFIG_YKB_BACKLIGHT
    REQUEST_GET_LUMISCRIPT_SLOT = 4U,
    REQUEST_SET_LUMISCRIPT_SLOT = 5U,
    REQUEST_GET_LUMISCRIPT_SLOT_INFO = 6U,
    REQUEST_CLEAR_LUMISCRIPT_SLOT = 7U,
    REQUEST_RENAME_LUMISCRIPT_SLOT = 8U,
#endif // CONFIG_YKB_BACKLIGHT

#if CONFIG_YKB_BATTSENSE
    REQUEST_GET_BATTERY_STATE = 9U,
#endif // CONFIG_YKB_BATTSENSE

#if CONFIG_SPLITLINK_SYNC_MASTER
    REQUEST_GET_SECONDARY_BATTERY_STATE = 10U,
#endif // CONFIG_SPLITLINK_SYNC_MASTER
};

enum response_type {
    RESPONSE_GET_FEATURES = 0U,
    RESPONSE_GET_VALUES = 1U,
    RESPONSE_GET_SETTINGS = 2U,
    RESPONSE_SET_SETTINGS_OK = 3U,

#if CONFIG_YKB_BACKLIGHT
    RESPONSE_GET_LUMISCRIPT_SLOT = 4U,
    RESPONSE_SET_LUMISCRIPT_SLOT_OK = 5U,
    RESPONSE_GET_LUMISCRIPT_SLOT_INFO = 6U,
    RESPONSE_CLEAR_LUMISCRIPT_SLOT_OK = 7U,
    RESPONSE_RENAME_LUMISCRIPT_SLOT_OK = 8U,
#endif // CONFIG_YKB_BACKLIGHT

#if CONFIG_YKB_BATTSENSE
    RESPONSE_GET_BATTERY_STATE = 9U,
#endif // CONFIG_YKB_BATTSENSE

#if CONFIG_SPLITLINK_SYNC_MASTER
    // CONFIG_SPLITLINK_SYNC_MASTER here means 2 things:
    // 1. It is a split keyboard.
    // 2. SplitlinkSync is enabled and battery state
    // is transmitted from slave to master.
    //
    // If slave keyboard doesn't have the battery
    // we will just return error or zeros I guess.
    RESPONSE_GET_SECONDARY_BATTERY_STATE = 10U,
#endif // CONFIG_SPLITLINK_SYNC_MASTER

    RESPONSE_ERROR = 255U,
};

#if CONFIG_YKB_BACKLIGHT

typedef struct __packed {
    uint8_t occupied;
    uint32_t size;
    char name[CONFIG_YKB_BL_SCRIPT_NAME_MAX_LEN + 1];
    uint8_t bytecode[CONFIG_YKB_BL_SCRIPT_SLOT_SIZE];
} vendor_hid_proto_script_slot_payload_t;

typedef struct __packed {
    uint16_t slot;
} vendor_hid_proto_script_slot_get_request_t;

typedef struct __packed {
    uint16_t slot;
    uint8_t occupied;
    uint32_t size;
    char name[CONFIG_YKB_BL_SCRIPT_NAME_MAX_LEN + 1];
} vendor_hid_proto_script_slot_info_t;

typedef struct __packed {
    uint16_t slot;
    vendor_hid_proto_script_slot_payload_t payload;
} vendor_hid_proto_script_slot_packet_t;

typedef struct __packed {
    uint16_t slot;
    char name[CONFIG_YKB_BL_SCRIPT_NAME_MAX_LEN + 1];
} vendor_hid_proto_script_slot_rename_request_t;

#define VENDOR_HID_MAX_DATA_LEN                                                \
    MAX(sizeof(kb_settings_t), sizeof(vendor_hid_proto_script_slot_packet_t))
#else
#define VENDOR_HID_MAX_DATA_LEN sizeof(kb_settings_t)
#endif // CONFIG_YKB_BACKLIGHT

typedef struct __packed {
    uint8_t type;
} vendor_hid_proto_request_header_t;

typedef struct __packed {
    vendor_hid_proto_request_header_t header;
    uint8_t data[VENDOR_HID_MAX_DATA_LEN];
} vendor_hid_proto_packet_t;

typedef int (*vendor_hid_send_packet_cb_t)(const uint8_t *data, size_t len,
                                           void *user_data);

typedef struct {
    uint8_t rx_buffer[sizeof(vendor_hid_proto_packet_t)];
    ykb_protocol_rx_state_t rx;
    struct k_work response_work;
    enum response_type current_response;
    vendor_hid_send_packet_cb_t send_packet;
    void *user_data;
    bool busy;
} vendor_hid_protocol_ctx_t;

int vendor_hid_protocol_init(vendor_hid_protocol_ctx_t *ctx,
                             vendor_hid_send_packet_cb_t send_packet,
                             void *user_data);

int vendor_hid_protocol_parse(vendor_hid_protocol_ctx_t *ctx,
                              const uint8_t *data, size_t len);

#endif // SUBSYS_VENDOR_HID_PROTOCOL_H
