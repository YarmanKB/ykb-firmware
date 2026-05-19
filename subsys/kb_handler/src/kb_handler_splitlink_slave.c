#include "kb_handler_internal.h"

#include <stdatomic.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "splitlink_handler/splitlink_handler.h"

LOG_MODULE_DECLARE(kb_handler);

BUILD_ASSERT(KEY_COUNT_SLAVE > 0,
             "KB_HANDLER_SPLITLINK_SLAVE requires kb-handler-key-count-slave");

static uint16_t values[KEY_COUNT_SLAVE] = {0};

static void send_values_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_values_work, send_values_work_handler);
static atomic_bool send_values_pending;

static void send_values_work_handler(struct k_work *work) {
    atomic_store_explicit(&send_values_pending, false, memory_order_relaxed);
    splitlink_handler_send_values(values, KEY_COUNT_SLAVE);
}

static void on_new_value(uint16_t idx, uint16_t value) {
    if (idx >= KEY_COUNT_SLAVE) {
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
    if (idx >= KEY_COUNT_SLAVE) {
        LOG_WRN("Ignoring out-of-range slave key event idx %u", idx);
        return;
    }

    values[idx] = pressed;
}

KSCAN_CB_DEFINE(kbh_sm) = {
    .on_new_value = on_new_value,
    .on_event = on_event,
};

void splitlink_handler_settings_received(const kb_settings_t *settings) {
    int err = kb_settings_apply(settings);
    if (err) {
        LOG_ERR("kb_settings_apply: %d", err);
    }
}

void splitlink_handler_scripts_manifest_received(
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

    splitlink_handler_request_scripts(&request);
}

void splitlink_handler_script_slot_received(
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

void splitlink_handler_on_connect() {
    //
    LOG_INF("SplitLink connected");
}

void splitlink_handler_on_disconnect() {
    //
    LOG_INF("SplitLink disconnected");
}

static int kb_handler_ss_init(void) {
    int err;

    err = kb_handler_check_kscans_ready();
    if (err) {
        return err;
    }

    err = kb_handler_validate_kscan_topology(KEY_COUNT_SLAVE);
    if (err) {
        return err;
    }

    return splitlink_handler_init();
}

SYS_INIT(kb_handler_ss_init, POST_KERNEL, CONFIG_KB_HANDLER_INIT_PRIORITY);
