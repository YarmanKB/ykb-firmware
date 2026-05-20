#include <subsys/kb_handler_internal_api.h>
#include <subsys/splitlink_sync.h>
#include <subsys/ykb_battsense.h>

#include <stdatomic.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(splitlink_sync);

BUILD_ASSERT(CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE > 0,
             "SPLITLINK_SYNC_SLAVE requires kb-handler-key-count-slave");

static uint16_t values[CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE] = {0};
static splitlink_battery_state_t battery_state = {0};

static void send_values_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_values_work, send_values_work_handler);
static atomic_bool send_values_pending;

static void send_values_work_handler(struct k_work *work) {
    atomic_store_explicit(&send_values_pending, false, memory_order_relaxed);
    splitlink_sync_send_values(values, CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE);
}

static void on_new_value(uint16_t idx, uint16_t value) {
    if (idx >= CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE) {
        LOG_WRN("Ignoring out-of-range slave key value idx %u", idx);
        return;
    }

    values[idx] = value;
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&send_values_pending, &expected,
                                                true, memory_order_relaxed,
                                                memory_order_relaxed)) {
        k_work_schedule(&send_values_work, K_MSEC(2));
    }
}

static void on_event(uint16_t idx, bool pressed) {
    if (idx >= CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE) {
        LOG_WRN("Ignoring out-of-range slave key event idx %u", idx);
        return;
    }

    values[idx] = pressed;
}

static void on_battery_state_changed(ykb_battsense_state_t state) {
    battery_state.percentage = state.percentage;
    battery_state.charge_status = (uint8_t)state.charge_status;
    splitlink_sync_send_battery_state(&battery_state);
}

#if CONFIG_YKB_BATTSENSE
static YKB_BATTSENSE_DEFINE(splitlink_sync_slave_battery_transport) = {
    .on_state_changed = on_battery_state_changed,
    .on_low_percentage = on_battery_state_changed,
    .on_critical_percentage = on_battery_state_changed,
};
#endif

void splitlink_sync_settings_received(const kb_settings_t *settings) {
    int err = kb_settings_apply(settings);
    if (err) {
        LOG_ERR("kb_settings_apply: %d", err);
    }
}

void splitlink_sync_scripts_manifest_received(
    const splitlink_script_manifest_t *manifest) {
    splitlink_script_request_t request = {0};
    ykb_backlight_script_slot_t slot_data;
    uint32_t local_crc32;
    int err;
    bool any_mismatch = false;
    uint16_t count = MIN(manifest->slot_count, CONFIG_YKB_BL_SCRIPT_SLOT_COUNT);

    request.slot_count = CONFIG_YKB_BL_SCRIPT_SLOT_COUNT;

    for (uint16_t slot = 0; slot < count; ++slot) {
        err = ykb_backlight_get_script_slot(slot, &slot_data);
        if (err) {
            LOG_ERR("ykb_backlight_get_script_slot(%u): %d", slot, err);
            request.bitmap[slot / 8U] |= BIT(slot % 8U);
            any_mismatch = true;
            continue;
        }

        err = ykb_backlight_get_script_slot_crc32(slot, &local_crc32);
        if (err) {
            LOG_ERR("ykb_backlight_get_script_slot_crc32(%u): %d", slot, err);
            request.bitmap[slot / 8U] |= BIT(slot % 8U);
            any_mismatch = true;
            continue;
        }

        if ((manifest->slots[slot].occupied ? true : false) != slot_data.occupied ||
            manifest->slots[slot].size != slot_data.size ||
            manifest->slots[slot].crc32 != local_crc32) {
            request.bitmap[slot / 8U] |= BIT(slot % 8U);
            any_mismatch = true;
        }
    }

    if (!any_mismatch) {
        return;
    }

    splitlink_sync_request_scripts(&request);
}

void splitlink_sync_script_slot_received(
    const splitlink_script_slot_packet_t *slot_packet) {
    int err;
    ykb_backlight_script_slot_t script_payload;

    if (!slot_packet) {
        return;
    }

    memcpy(&script_payload, &slot_packet->payload, sizeof(script_payload));
    err = ykb_backlight_set_script_slot(slot_packet->slot, &script_payload);
    if (err) {
        LOG_ERR("ykb_backlight_set_script_slot(%u): %d", slot_packet->slot, err);
    }
}

void splitlink_sync_on_connect() {
    LOG_INF("SplitLink connected");

#if CONFIG_YKB_BATTSENSE
    ykb_battsense_state_t state = {0};
    if (!ykb_battsense_get_state(&state)) {
        on_battery_state_changed(state);
    }
#endif
}

void splitlink_sync_on_disconnect() {
    LOG_INF("SplitLink disconnected");
}

void splitlink_sync_battery_state_received(const splitlink_battery_state_t *state) {
    ARG_UNUSED(state);
}

int splitlink_sync_slave_attach_kb_handler(void) {
    int err;

    err = kb_handler_check_kscans_ready();
    if (err) {
        return err;
    }

    err = kb_handler_validate_kscan_topology(CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE);
    if (err) {
        return err;
    }

    return splitlink_sync_init();
}

void splitlink_sync_slave_on_local_key_event(uint16_t idx, bool pressed) {
    on_event(idx, pressed);
}

void splitlink_sync_slave_on_local_value(uint16_t idx, uint16_t value) {
    on_new_value(idx, value);
}
