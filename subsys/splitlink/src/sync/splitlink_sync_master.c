#include "splitlink_sync_private.h"
#include <subsys/splitlink_sync.h>

#include <subsys/bt_connect.h>
#include <subsys/kb_handler_internal_api.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <string.h>

LOG_MODULE_DECLARE(splitlink_sync);

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

static void
splitlink_build_script_manifest(splitlink_script_manifest_t *manifest) {
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

void splitlink_sync_values_received(uint16_t *slave_values, uint16_t count) {
    kb_handler_core_handle_slave_values(slave_values, count);
}

void splitlink_sync_on_connect() {
    LOG_INF("SplitLink slave connected");

    STRUCT_SECTION_FOREACH(splitlink_sync_cb, iter) {
        if (iter->on_connected) {
            iter->on_connected();
        }
    }

    if (!kb_handler_core_get_settings_snapshot(&splitlink_settings_tx)) {
        splitlink_sync_send_settings(&splitlink_settings_tx);
    }

    splitlink_build_script_manifest(&splitlink_script_manifest_tx);
    splitlink_sync_send_scripts_manifest(&splitlink_script_manifest_tx);
}

void splitlink_sync_on_disconnect() {
    LOG_WRN("SplitLink slave disconnected");
    kb_handler_core_handle_slave_reset();

    STRUCT_SECTION_FOREACH(splitlink_sync_cb, iter) {
        if (iter->on_disconnected) {
            iter->on_disconnected();
        }
    }
}

void splitlink_sync_on_settings_update(const kb_settings_t *settings) {
    memcpy(&splitlink_settings_tx, settings, sizeof(splitlink_settings_tx));
    splitlink_sync_send_settings(&splitlink_settings_tx);
}

void splitlink_sync_scripts_request_received(
    const splitlink_script_request_t *request) {
    uint16_t count = MIN(request->slot_count, CONFIG_YKB_BL_SCRIPT_SLOT_COUNT);

    for (uint16_t slot = 0; slot < count; ++slot) {
        if ((request->bitmap[slot / 8U] & BIT(slot % 8U)) == 0U) {
            continue;
        }

        splitlink_sync_queue_script_slot_sync(slot);
    }
}

void splitlink_sync_on_script_slot_update(uint16_t slot) {
    splitlink_sync_queue_script_slot_sync(slot);
}

void splitlink_sync_battery_state_received(
    const splitlink_battery_state_t *state) {
    ykb_battsense_state_t slave_state = {0};

    if (!state) {
        return;
    }

    slave_state.percentage = state->percentage;
    slave_state.charge_status = (enum charger_status)state->charge_status;

    STRUCT_SECTION_FOREACH(splitlink_sync_cb, iter) {
        if (iter->on_slave_battery_state) {
            iter->on_slave_battery_state(&slave_state);
        }
    }
}

int splitlink_sync_master_attach_kb_handler(void) {
    int err = splitlink_sync_init();

    if (err) {
        return err;
    }

    err = kb_handler_register_settings_update_cb(
        splitlink_sync_on_settings_update);
    if (err) {
        return err;
    }

    err = ykb_backlight_register_script_slot_update_cb(
        splitlink_sync_on_script_slot_update);
    if (err) {
        return err;
    }

    return kb_handler_core_init();
}

void splitlink_sync_master_on_local_key_event(uint16_t idx, bool pressed) {
    kb_handler_core_handle_key_event(idx, pressed);
}

void splitlink_sync_master_on_local_value(uint16_t idx, uint16_t value) {
    kb_handler_core_handle_value(idx, value);
}
