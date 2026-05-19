#include "kb_handler_internal.h"

#include "splitlink_handler/splitlink_handler.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <string.h>

LOG_MODULE_DECLARE(kb_handler);

static kb_settings_t splitlink_settings_tx;
static splitlink_script_manifest_t splitlink_script_manifest_tx;

static uint32_t splitlink_script_slot_crc32(uint16_t slot) {
    uint32_t crc32 = 0;
    int err = ykb_backlight_get_script_slot_crc32(slot, &crc32);

    if (err) {
        LOG_ERR("ykb_backlight_get_script_slot_crc32(%u): %d", slot, err);
        return 0;
    }

    return crc32;
}

static void splitlink_build_script_manifest(
    splitlink_script_manifest_t *manifest) {
    ykb_backlight_script_slot_t slot_data;

    memset(manifest, 0, sizeof(*manifest));
    manifest->slot_count = CONFIG_YKB_BL_SCRIPT_SLOT_COUNT;

    for (uint16_t slot = 0; slot < manifest->slot_count; ++slot) {
        if (ykb_backlight_get_script_slot(slot, &slot_data)) {
            continue;
        }

        manifest->slots[slot].occupied = slot_data.occupied ? 1U : 0U;
        manifest->slots[slot].size = slot_data.size;
        manifest->slots[slot].crc32 = splitlink_script_slot_crc32(slot);
    }
}

KSCAN_CB_DEFINE(kbh_sm) = {
    .on_event = kb_handler_core_handle_key_event,
    .on_new_value = kb_handler_core_handle_value,
};

void splitlink_handler_values_received(uint16_t *slave_values, uint16_t count) {
    kb_handler_core_handle_slave_values(slave_values, count);
}

void splitlink_handler_on_connect() {
    LOG_INF("SplitLink slave connected");

    if (!kb_handler_core_get_settings_snapshot(&splitlink_settings_tx)) {
        splitlink_handler_send_settings(&splitlink_settings_tx);
    }

    splitlink_build_script_manifest(&splitlink_script_manifest_tx);
    splitlink_handler_send_scripts_manifest(&splitlink_script_manifest_tx);
}

void splitlink_handler_on_disconnect() {
    LOG_WRN("SplitLink slave disconnected");
    kb_handler_core_handle_slave_reset();
}

void kb_handler_impl_after_settings_update(const kb_settings_t *settings) {
    memcpy(&splitlink_settings_tx, settings, sizeof(splitlink_settings_tx));
    splitlink_handler_send_settings(&splitlink_settings_tx);
}

void splitlink_handler_scripts_request_received(
    const splitlink_script_request_t *request) {
    uint16_t count = MIN(request->slot_count, CONFIG_YKB_BL_SCRIPT_SLOT_COUNT);

    for (uint16_t slot = 0; slot < count; ++slot) {
        if ((request->bitmap[slot / 8U] & BIT(slot % 8U)) == 0U) {
            continue;
        }

        splitlink_handler_queue_script_slot_sync(slot);
    }
}

void ykb_backlight_on_script_slot_update(uint16_t slot) {
    splitlink_handler_queue_script_slot_sync(slot);
}

static int kb_handler_sm_init(void) {
    int err = splitlink_handler_init();

    if (err) {
        return err;
    }

    return kb_handler_core_init();
}

SYS_INIT(kb_handler_sm_init, POST_KERNEL, CONFIG_KB_HANDLER_INIT_PRIORITY);
