#include <subsys/kb_settings.h>

#include "generated_default_settings.h"

#ifdef CONFIG_YKB_BACKLIGHT
#include <subsys/ykb_backlight.h>
#endif // CONFIG_YKB_BACKLIGHT
#ifdef CONFIG_YKB_BATTSENSE
#include <subsys/ykb_battsense.h>
#endif // CONFIG_YKB_BATTSENSE
#ifdef CONFIG_YKB_POWER
#include <subsys/ykb_power.h>
#endif // CONFIG_YKB_POWER

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/toolchain.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KB_SETTINGS_NS "kb"
#define KB_SETTINGS_ITEM "blob"
#define KB_SETTINGS_KEY KB_SETTINGS_NS "/" KB_SETTINGS_ITEM

LOG_MODULE_REGISTER(kb_settings, CONFIG_KB_SETTINGS_LOG_LEVEL);

// Increment every time kb_settings_image_t or it's contents change
#define KB_SETTINGS_IMAGE_VERSION 1

BUILD_ASSERT(GENERATED_DEFAULT_SETTINGS_KEY_COUNT == TOTAL_KEY_COUNT,
             "generated default settings should match TOTAL_KEY_COUNT");

typedef struct {
    uint16_t version;
    kb_settings_t settings;
} kb_settings_image_t;

static kb_settings_t kb_settings;
static bool settings_registered = false;
static bool successfully_loaded = false;
static kb_settings_t notify_snapshot;
static kb_settings_image_t load_img;
static kb_settings_image_t save_img;

static K_MUTEX_DEFINE(kb_settings_mut);

static void
kb_settings_handle_on_update_snapshot(const kb_settings_t *settings) {
    STRUCT_SECTION_FOREACH(kb_settings_cb, callbacks) {
        if (callbacks->on_update) {
            callbacks->on_update(settings);
        }
    }
}

static int kb_settings_load_defaults(void) {
    int err = 0;

    k_mutex_lock(&kb_settings_mut, K_FOREVER);

    kb_settings.mode = KB_MODE_NORMAL;

    memcpy(kb_settings.mappings_layer1,
           generated_default_settings_keymap_layer1,
           sizeof(kb_settings.mappings_layer1));
    memcpy(kb_settings.mappings_layer2,
           generated_default_settings_keymap_layer2,
           sizeof(kb_settings.mappings_layer2));
    memcpy(kb_settings.mappings_layer3,
           generated_default_settings_keymap_layer3,
           sizeof(kb_settings.mappings_layer3));
    memcpy(&kb_settings.mouseemu, &generated_default_settings_mouseemu,
           sizeof(kb_settings.mouseemu));

    if (generated_default_settings_fn_shortcuts_count >
        CONFIG_KB_SETTINGS_FN_SHORTCUTS_MAX) {
        err = -E2BIG;
        goto cleanup;
    }

    kb_settings.fn_shortcuts_count =
        (uint8_t)generated_default_settings_fn_shortcuts_count;
    memset(kb_settings.fn_shortcuts, 0, sizeof(kb_settings.fn_shortcuts));
    if (generated_default_settings_fn_shortcuts_count > 0U) {
        memcpy(kb_settings.fn_shortcuts,
               generated_default_settings_fn_shortcuts,
               generated_default_settings_fn_shortcuts_count *
                   sizeof(kb_fn_shortcut_t));
    }

    memcpy(kb_settings.thresholds, generated_default_settings_thresholds,
           sizeof(kb_settings.thresholds));
    memcpy(kb_settings.minimums, generated_default_settings_minimums,
           sizeof(kb_settings.minimums));
    memcpy(kb_settings.maximums, generated_default_settings_maximums,
           sizeof(kb_settings.maximums));
    memcpy(kb_settings.deadzones, generated_default_settings_deadzones,
           sizeof(kb_settings.deadzones));

#if CONFIG_YKB_BACKLIGHT
    const ykb_backlight_settings_t *default_backlight_settings =
        ykb_backlight_get_default_settings();
    memcpy(&kb_settings.backlight, default_backlight_settings,
           sizeof(kb_settings.backlight));
#endif // CONFIG_YKB_BACKLIGHT

#if CONFIG_YKB_BATTSENSE
    const ykb_battsense_settings_t *default_battsense_settings =
        ykb_battsense_get_default_settings();
    memcpy(&kb_settings.battsense, default_battsense_settings,
           sizeof(kb_settings.battsense));
#endif // CONFIG_YKB_BATTSENSE

#if CONFIG_YKB_POWER
    const ykb_power_settings_t *default_power_settings =
        ykb_power_get_default_settings();
    memcpy(&kb_settings.power, default_power_settings,
           sizeof(kb_settings.power));
#endif // CONFIG_YKB_POWER

cleanup:

    k_mutex_unlock(&kb_settings_mut);

    return err;
}

int kb_settings_handler_set(const char *key, size_t len,
                            settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(key, KB_SETTINGS_ITEM) != 0) {
        return -ENOENT;
    }

    const size_t kb_settings_img_size = sizeof(kb_settings_image_t);
    if (len != kb_settings_img_size) {
        LOG_ERR("Keyboard settings image size mismatch: got %zu, want %zu", len,
                kb_settings_img_size);
        return -EINVAL;
    }

    ssize_t rlen = read_cb(cb_arg, &load_img, sizeof(load_img));

    if (rlen < 0) {
        LOG_ERR("Keyboard settings read_cb error: %d", (int)rlen);
        return -EINVAL;
    }

    if ((size_t)rlen != sizeof(load_img)) {
        LOG_ERR("Keyboard settings truncated: %zd", rlen);
        return -EINVAL;
    }

    if (load_img.version != KB_SETTINGS_IMAGE_VERSION) {
        LOG_ERR("Keyboad settings image version mismatch: got %u, want %u",
                load_img.version, KB_SETTINGS_IMAGE_VERSION);
        return -EINVAL;
    }

    k_mutex_lock(&kb_settings_mut, K_FOREVER);

    memcpy(&kb_settings, &load_img.settings, sizeof(kb_settings_t));
    memcpy(&notify_snapshot, &kb_settings, sizeof(kb_settings_t));
    successfully_loaded = true;

    k_mutex_unlock(&kb_settings_mut);

    kb_settings_handle_on_update_snapshot(&notify_snapshot);

    return 0;
}

int kb_settings_handler_export(int (*export_func)(const char *name,
                                                  const void *val,
                                                  size_t val_len)) {
    save_img = (kb_settings_image_t){
        .version = KB_SETTINGS_IMAGE_VERSION,
    };
    k_mutex_lock(&kb_settings_mut, K_FOREVER);
    memcpy(&save_img.settings, &kb_settings, sizeof(kb_settings_t));
    k_mutex_unlock(&kb_settings_mut);
    return export_func(KB_SETTINGS_ITEM, &save_img, sizeof(save_img));
}

static struct settings_handler kb_settings_handler = {
    .name = KB_SETTINGS_NS,
    .h_set = kb_settings_handler_set,
    .h_export = kb_settings_handler_export,
};

static void kb_settings_save() {
    if (!settings_registered) {
        LOG_WRN(
            "Attempt to save kb_settings but settings API was not registered.");
        return;
    }
    save_img = (kb_settings_image_t){
        .version = KB_SETTINGS_IMAGE_VERSION,
    };
    memcpy(&save_img.settings, &kb_settings, sizeof(kb_settings));
    int err = settings_save_one(KB_SETTINGS_KEY, &save_img, sizeof(save_img));
    if (err) {
        LOG_WRN("Could not save keyboard settings: %d", err);
        return;
    }
    LOG_INF("Keyboard settings saved.");
}

static int kb_settings_init(void) {
    int err;
    int res;
    successfully_loaded = false;

    if (!settings_registered) {
        err = settings_subsys_init();
        if (err) {
            LOG_ERR("settings_subsys_init: %d", err);
            goto load_defaults;
        }

        err = settings_register(&kb_settings_handler);
        if (err) {
            LOG_ERR("settings_register: %d", err);
            goto load_defaults;
        }
        settings_registered = true;
    }

    err = settings_load_subtree(KB_SETTINGS_NS);

    if (err || !successfully_loaded) {
        LOG_WRN("No valid keyboard settings found (err %d). Loading defaults.",
                err);
        goto load_defaults;
    }

    return 0;

load_defaults:

    res = kb_settings_load_defaults();
    if (res) {
        LOG_ERR("kb_settings_load_defaults: %d", res);
        k_panic();
        return res;
    }

    kb_settings_save();

    k_mutex_lock(&kb_settings_mut, K_FOREVER);
    memcpy(&notify_snapshot, &kb_settings, sizeof(kb_settings_t));
    k_mutex_unlock(&kb_settings_mut);

    kb_settings_handle_on_update_snapshot(&notify_snapshot);

    return err;
}

SYS_INIT(kb_settings_init, POST_KERNEL, CONFIG_KB_SETTINGS_INIT_PRIORITY);

int kb_settings_get(kb_settings_t *settings) {
    if (!settings) {
        return -EINVAL;
    }
    k_mutex_lock(&kb_settings_mut, K_FOREVER);

    memcpy(settings, &kb_settings, sizeof(kb_settings));

    k_mutex_unlock(&kb_settings_mut);

    return 0;
}

int kb_settings_apply(const kb_settings_t *settings) {
    if (!settings) {
        return -EINVAL;
    }
    k_mutex_lock(&kb_settings_mut, K_FOREVER);

    memcpy(&kb_settings, settings, sizeof(kb_settings_t));
    memcpy(&notify_snapshot, settings, sizeof(kb_settings_t));

    k_mutex_unlock(&kb_settings_mut);

    kb_settings_save();

    kb_settings_handle_on_update_snapshot(&notify_snapshot);

    return 0;
}
