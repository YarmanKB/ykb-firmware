#include "kb_handler_internal.h"

#include "generated_kb_handler_layout.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <string.h>

LOG_MODULE_DECLARE(kb_handler);

#define KSCAN_DEV_AND_COMMA(node_id, prop, idx)                                \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

static const struct device *const kscans[] = {DT_FOREACH_PROP_ELEM(
    DT_PATH(zephyr_user), kb_handler_kscans, KSCAN_DEV_AND_COMMA)};

BUILD_ASSERT(KEY_COUNT + KEY_COUNT_SLAVE == TOTAL_KEY_COUNT,
             "kb_handler key counts should sum to TOTAL_KEY_COUNT");
BUILD_ASSERT(TOTAL_KEY_COUNT > 0,
             "TOTAL_KEY_COUNT should be greater than zero");
BUILD_ASSERT(GENERATED_KB_HANDLER_KEY_COUNT == TOTAL_KEY_COUNT,
             "generated kb_handler layout should match TOTAL_KEY_COUNT");

size_t kb_handler_kscan_count(void) { return ARRAY_SIZE(kscans); }

const struct device *kb_handler_get_kscan(size_t idx) {
    if (idx >= ARRAY_SIZE(kscans)) {
        return NULL;
    }

    return kscans[idx];
}

int kb_handler_check_kscans_ready(void) {
    for (size_t i = 0; i < ARRAY_SIZE(kscans); ++i) {
        if (!device_is_ready(kscans[i])) {
            LOG_ERR("KScan device '%s' is not ready", kscans[i]->name);
            return -ENODEV;
        }
    }

    return 0;
}

int kb_handler_validate_kscan_topology(uint16_t expected_key_count) {
    int expected_offset = 0;
    int total_key_count = 0;

    for (size_t i = 0; i < ARRAY_SIZE(kscans); ++i) {
        int offset = kscan_get_idx_offset(kscans[i]);
        int key_amount = kscan_get_key_amount(kscans[i]);

        if (offset < 0) {
            LOG_ERR("Unable to get idx offset for KScan %u (err %d)",
                    (unsigned)i, offset);
            return offset;
        }
        if (key_amount < 0) {
            LOG_ERR("Unable to get key amount for KScan %u (err %d)",
                    (unsigned)i, key_amount);
            return key_amount;
        }

        total_key_count += key_amount;

        if (i != 0U) {
            if (expected_offset < offset) {
                LOG_ERR("Key indices gap between KScans %u and %u "
                        "(expected offset %d, got %d)",
                        (unsigned)i - 1U, (unsigned)i, expected_offset, offset);
                return -EINVAL;
            }
            if (expected_offset > offset) {
                LOG_ERR("Key indices intersection between KScans %u and %u "
                        "(expected offset %d, got %d)",
                        (unsigned)i - 1U, (unsigned)i, expected_offset, offset);
                return -EINVAL;
            }
        }

        expected_offset = offset + key_amount;
    }

    if (total_key_count != expected_key_count) {
        LOG_ERR("KScans total key count != expected key count (%d != %u)",
                total_key_count, expected_key_count);
        return -EINVAL;
    }

    return 0;
}

int kb_handler_get_default_keymap_layer1(uint8_t *buffer) {
    if (buffer) {
        memcpy(buffer, generated_kb_handler_default_keymap_layer1,
               sizeof(generated_kb_handler_default_keymap_layer1));
    }

    return 0;
}

int kb_handler_get_default_keymap_layer2(uint8_t *buffer) {
    if (buffer) {
        memcpy(buffer, generated_kb_handler_default_keymap_layer2,
               sizeof(generated_kb_handler_default_keymap_layer2));
    }

    return 0;
}

int kb_handler_get_default_keymap_layer3(uint8_t *buffer) {
    if (buffer) {
        memcpy(buffer, generated_kb_handler_default_keymap_layer3,
               sizeof(generated_kb_handler_default_keymap_layer3));
    }

    return 0;
}

int kb_handler_get_default_mouseemu(kb_mouseemu_settings_t *buffer) {
    if (buffer) {
        memcpy(buffer, &generated_kb_handler_default_mouseemu,
               sizeof(generated_kb_handler_default_mouseemu));
    }

    return 0;
}

size_t kb_handler_get_default_fn_shortcuts(const kb_fn_shortcut_t **shortcuts) {
    if (shortcuts) {
        *shortcuts = generated_kb_handler_default_fn_shortcuts;
    }

    return generated_kb_handler_default_fn_shortcuts_count;
}

int kb_handler_get_default_thresholds(uint16_t *buffer) {
    if (buffer) {
        memcpy(buffer, generated_kb_handler_default_thresholds,
               sizeof(generated_kb_handler_default_thresholds));
    }

    return 0;
}

int kb_handler_get_default_minimums(uint16_t *buffer) {
    if (buffer) {
        memcpy(buffer, generated_kb_handler_default_minimums,
               sizeof(generated_kb_handler_default_minimums));
    }

    return 0;
}

int kb_handler_get_default_maximums(uint16_t *buffer) {
    if (buffer) {
        memcpy(buffer, generated_kb_handler_default_maximums,
               sizeof(generated_kb_handler_default_maximums));
    }

    return 0;
}
