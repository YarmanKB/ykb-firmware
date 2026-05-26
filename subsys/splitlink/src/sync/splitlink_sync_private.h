#ifndef SPLITLINK_SYNC_PRIVATE_H
#define SPLITLINK_SYNC_PRIVATE_H

#include <subsys/kb_settings.h>
#include <subsys/ykb_backlight.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct __packed {
    uint8_t occupied;
    uint32_t size;
    uint32_t crc32;
} splitlink_script_manifest_entry_t;

typedef struct __packed {
    uint16_t slot_count;
    splitlink_script_manifest_entry_t slots[CONFIG_YKB_BL_SCRIPT_SLOT_COUNT];
} splitlink_script_manifest_t;

typedef struct __packed {
    uint16_t slot_count;
    uint8_t bitmap[(CONFIG_YKB_BL_SCRIPT_SLOT_COUNT + 7U) / 8U];
} splitlink_script_request_t;

typedef struct __packed {
    uint16_t slot;
    ykb_backlight_script_slot_t payload;
} splitlink_script_slot_packet_t;

typedef struct __packed {
    uint8_t percentage;
    uint8_t charge_status;
} splitlink_battery_state_t;

int splitlink_sync_init(void);

void splitlink_sync_protocol_on_connect(void);
void splitlink_sync_protocol_on_disconnect(void);
void splitlink_sync_protocol_on_receive(uint8_t *data, size_t data_len);

void splitlink_sync_on_connect(void);
void splitlink_sync_on_disconnect(void);

void splitlink_sync_values_received(uint16_t *values, uint16_t count);
void splitlink_sync_send_values(uint16_t *values, uint16_t count);

void splitlink_sync_settings_received(const kb_settings_t *settings);
void splitlink_sync_send_settings(const kb_settings_t *settings);

void splitlink_sync_scripts_manifest_received(
    const splitlink_script_manifest_t *manifest);
void splitlink_sync_send_scripts_manifest(
    const splitlink_script_manifest_t *manifest);

void splitlink_sync_scripts_request_received(
    const splitlink_script_request_t *request);
void splitlink_sync_request_scripts(const splitlink_script_request_t *request);

void splitlink_sync_script_slot_received(
    const splitlink_script_slot_packet_t *slot_packet);
void splitlink_sync_send_script_slot(
    const splitlink_script_slot_packet_t *slot_packet);
void splitlink_sync_queue_script_slot_sync(uint16_t slot);

void splitlink_sync_battery_state_received(
    const splitlink_battery_state_t *state);
void splitlink_sync_send_battery_state(const splitlink_battery_state_t *state);

#endif // SPLITLINK_SYNC_PRIVATE_H
