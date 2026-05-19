#ifndef SPLITLINK_HANDLER_H
#define SPLITLINK_HANDLER_H

#include <subsys/kb_settings.h>
#include <subsys/ykb_backlight.h>

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

int splitlink_handler_init();

void splitlink_handler_on_connect();

void splitlink_handler_on_disconnect();

void splitlink_handler_values_received(uint16_t *values, uint16_t count);

void splitlink_handler_send_values(uint16_t *values, uint16_t count);

void splitlink_handler_settings_received(const kb_settings_t *settings);

void splitlink_handler_send_settings(const kb_settings_t *settings);

void splitlink_handler_scripts_manifest_received(
    const splitlink_script_manifest_t *manifest);

void splitlink_handler_send_scripts_manifest(
    const splitlink_script_manifest_t *manifest);

void splitlink_handler_scripts_request_received(
    const splitlink_script_request_t *request);

void splitlink_handler_request_scripts(const splitlink_script_request_t *request);

void splitlink_handler_script_slot_received(
    const splitlink_script_slot_packet_t *slot_packet);

void splitlink_handler_send_script_slot(
    const splitlink_script_slot_packet_t *slot_packet);

void splitlink_handler_queue_script_slot_sync(uint16_t slot);

#endif // SPLITLINK_HANDLER_H
