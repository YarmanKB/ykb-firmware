#include <subsys/ykb_backlight.h>

#include "generated_backlight_resources.h"
#include "lumiscript_vm.h"

#include <subsys/kb_settings.h>
#include <subsys/zephyr_user_helpers.h>

#include <drivers/kscan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ykb_backlight, CONFIG_YKB_BACKLIGHT_LOG_LEVEL);

#define YKB_BACKLIGHT_SETTINGS_NS "ykb_bl"
#define YKB_BACKLIGHT_SETTINGS_SLOT_PREFIX "slot_"
#define YKB_BACKLIGHT_SCRIPT_IMAGE_VERSION 1U

static const struct device *led_strip = Z_USER_DEV(ykb_backlight);
static const ykb_backlight_layout_t *layout;
static const ykb_backlight_script_slots_t *default_script_slots =
    &default_backlight_script_slots;

static const size_t led_count =
    DT_PROP(Z_USER_PROP(ykb_backlight), chain_length);
static const size_t local_key_capacity = CONFIG_KB_SETTINGS_KEY_COUNT;

static const double max_absolute_brightness =
    ((double)YKB_BACKLIGHT_MAX_ABS_BRIGHTNESS_PERCENT) / 100.0;
BUILD_ASSERT(YKB_BACKLIGHT_MAX_ABS_BRIGHTNESS_PERCENT >= 1 &&
                 YKB_BACKLIGHT_MAX_ABS_BRIGHTNESS_PERCENT <= 100,
             "ykb-backlight-max-abs-brightness should be in the range [1-100]");
BUILD_ASSERT(CONFIG_KB_SETTINGS_KEY_COUNT > 0,
             "KB_SETTINGS_KEY_COUNT should be greater than zero");

static struct k_thread ykb_backlight_thread;
static K_THREAD_STACK_DEFINE(ykb_backlight_thread_stack,
                             CONFIG_YKB_BL_THREAD_STACK_SIZE);

static K_MUTEX_DEFINE(ykb_bl_mut);

static int64_t prev_update = 0;
static float cur_speed = 1.0;
static float cur_brightness = 1.0;
static bool on = true;
static bool scripts_registered = false;
static bool scripts_loaded = false;

static bool script_loaded = false;

static uint16_t press[CONFIG_KB_SETTINGS_KEY_COUNT] = {0};
static bool pressed[CONFIG_KB_SETTINGS_KEY_COUNT] = {0};

static uint32_t thread_sleep_time = DEFAULT_THREAD_SLEEP_MS;

static struct led_rgb
    _buffer1[DT_PROP(Z_USER_PROP(ykb_backlight), chain_length)] = {0};
static struct led_rgb
    _buffer2[DT_PROP(Z_USER_PROP(ykb_backlight), chain_length)] = {0};
static struct led_rgb *buf_front = _buffer1;
static struct led_rgb *buf_back = _buffer2;

typedef struct {
    uint16_t version;
    ykb_backlight_script_slot_t slot;
} ykb_backlight_script_slot_image_t;

static ykb_backlight_script_slots_t script_slots;
static const ykb_backlight_settings_t default_backlight_settings = {
    .on = true,
    .active_script_index = (CONFIG_YKB_BL_SCRIPT_SLOT_COUNT > 2) ? 2 : 0,
    .speed = 1.0f,
    .brightness = 1.0f,
    .thread_sleep_ms = DEFAULT_THREAD_SLEEP_MS,
};

static int ykb_backlight_load_default_scripts(void) {
    memcpy(&script_slots, default_script_slots, sizeof(script_slots));
    scripts_loaded = true;
    return 0;
}

static int ykb_backlight_settings_slot_name(uint16_t slot, char *name,
                                            size_t name_len) {
    int written =
        snprintf(name, name_len, YKB_BACKLIGHT_SETTINGS_SLOT_PREFIX "%u", slot);

    if (written < 0 || (size_t)written >= name_len) {
        return -EINVAL;
    }

    return 0;
}

static int ykb_backlight_settings_slot_key(uint16_t slot, char *key,
                                           size_t key_len) {
    int written =
        snprintf(key, key_len, "%s/" YKB_BACKLIGHT_SETTINGS_SLOT_PREFIX "%u",
                 YKB_BACKLIGHT_SETTINGS_NS, slot);

    if (written < 0 || (size_t)written >= key_len) {
        return -EINVAL;
    }

    return 0;
}

static int ykb_backlight_save_script_slot(uint16_t slot) {
    char key[SETTINGS_FULL_NAME_LEN];
    ykb_backlight_script_slot_image_t save_img = {
        .version = YKB_BACKLIGHT_SCRIPT_IMAGE_VERSION,
    };
    int err;

    if (slot >= ARRAY_SIZE(script_slots.slots)) {
        return -ERANGE;
    }

    err = ykb_backlight_settings_slot_key(slot, key, sizeof(key));
    if (err) {
        return err;
    }

    memcpy(&save_img.slot, &script_slots.slots[slot], sizeof(save_img.slot));
    return settings_save_one(key, &save_img, sizeof(save_img));
}

static int ykb_backlight_script_handler_set(const char *key, size_t len,
                                            settings_read_cb read_cb,
                                            void *cb_arg) {
    const char *slot_key = key;
    char *endptr = NULL;
    unsigned long slot_idx_ul;
    ykb_backlight_script_slot_image_t load_img;

    if (strncmp(slot_key, YKB_BACKLIGHT_SETTINGS_SLOT_PREFIX,
                strlen(YKB_BACKLIGHT_SETTINGS_SLOT_PREFIX)) != 0) {
        return -ENOENT;
    }

    slot_key += strlen(YKB_BACKLIGHT_SETTINGS_SLOT_PREFIX);
    if (*slot_key == '\0') {
        return -EINVAL;
    }

    slot_idx_ul = strtoul(slot_key, &endptr, 10);
    if (!endptr || *endptr != '\0' ||
        slot_idx_ul >= ARRAY_SIZE(script_slots.slots)) {
        LOG_ERR("Invalid backlight script slot key '%s'", key);
        return -EINVAL;
    }

    if (len != sizeof(load_img)) {
        LOG_ERR("Backlight script slot image size mismatch: got %zu, want %zu",
                len, sizeof(load_img));
        return -EINVAL;
    }

    ssize_t rlen = read_cb(cb_arg, &load_img, sizeof(load_img));
    if (rlen < 0) {
        LOG_ERR("Backlight script read_cb error: %d", (int)rlen);
        return -EINVAL;
    }

    if ((size_t)rlen != sizeof(load_img)) {
        LOG_ERR("Backlight script slot image truncated: %zd", rlen);
        return -EINVAL;
    }

    if (load_img.version != YKB_BACKLIGHT_SCRIPT_IMAGE_VERSION) {
        LOG_ERR("Backlight script image version mismatch: got %u, want %u",
                load_img.version, YKB_BACKLIGHT_SCRIPT_IMAGE_VERSION);
        return -EINVAL;
    }

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    memcpy(&script_slots.slots[slot_idx_ul], &load_img.slot,
           sizeof(load_img.slot));
    scripts_loaded = true;
    k_mutex_unlock(&ykb_bl_mut);

    return 0;
}

static int ykb_backlight_script_handler_export(
    int (*export_func)(const char *name, const void *val, size_t val_len)) {
    int err = 0;

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    for (uint16_t slot = 0; slot < ARRAY_SIZE(script_slots.slots); ++slot) {
        char name[SETTINGS_FULL_NAME_LEN];
        ykb_backlight_script_slot_image_t save_img = {
            .version = YKB_BACKLIGHT_SCRIPT_IMAGE_VERSION,
        };

        err = ykb_backlight_settings_slot_name(slot, name, sizeof(name));
        if (err) {
            break;
        }

        memcpy(&save_img.slot, &script_slots.slots[slot],
               sizeof(save_img.slot));
        err = export_func(name, &save_img, sizeof(save_img));
        if (err) {
            break;
        }
    }
    k_mutex_unlock(&ykb_bl_mut);

    return err;
}

static struct settings_handler ykb_backlight_script_handler = {
    .name = YKB_BACKLIGHT_SETTINGS_NS,
    .h_set = ykb_backlight_script_handler_set,
    .h_export = ykb_backlight_script_handler_export,
};

static ykb_backlight_script_slot_update_cb_t script_slot_update_cb;

int ykb_backlight_register_script_slot_update_cb(
    ykb_backlight_script_slot_update_cb_t cb) {
    if (script_slot_update_cb && script_slot_update_cb != cb) {
        return -EALREADY;
    }

    script_slot_update_cb = cb;
    return 0;
}

static void ykb_backlight_save_scripts(void) {
    int err = 0;

    if (!scripts_registered) {
        LOG_WRN(
            "Attempt to save backlight scripts before settings registration");
        return;
    }

    for (uint16_t slot = 0; slot < ARRAY_SIZE(script_slots.slots); ++slot) {
        err = ykb_backlight_save_script_slot(slot);
        if (err) {
            LOG_WRN("Could not save backlight script slot %u: %d", slot, err);
            return;
        }
    }

    LOG_INF("Backlight scripts saved.");
}

static inline uint8_t apply_brightness(uint8_t color) {
    double result =
        (double)color * max_absolute_brightness * (double)cur_brightness;
    return (uint8_t)ceil(result);
}

static inline void clear_state() {
    memset(buf_back, 0, sizeof(struct led_rgb) * led_count);
    struct led_rgb *tmp = buf_front;
    buf_front = buf_back;
    buf_back = tmp;

    int err = led_strip_update_rgb(led_strip, buf_front, led_count);
    if (err) {
        LOG_ERR("clear_state: led_strip_update_rgb: %d", err);
        return;
    }

    if (script_loaded) {
        err = lumiscript_reset_state();
        if (err) {
            LOG_ERR("lumiscript_reset_state: %d", err);
            return;
        }
    }
}

static void ykb_backlight_thread_handler(void *a, void *b, void *c) {
    while (true) {
        k_mutex_lock(&ykb_bl_mut, K_FOREVER);
        if (!script_loaded) {
            goto cont;
        }
        if (!on) {
            goto cont;
        }

        int64_t cur_uptime = k_uptime_get();
        int64_t dt = cur_uptime - prev_update;
        prev_update = cur_uptime;

        lumi_vm_inputs inputs = {
            .dt = dt,
            .speed = cur_speed,
        };
        int err = lumiscript_run_update(&inputs);
        if (err) {
            LOG_ERR("Lumiscirpt update: %d", err);
            goto cont;
        }

        for (uint16_t i = 0; i < layout->key_count; ++i) {
            lumi_vm_output output;
            inputs.x = layout->x_coordinates[i];
            inputs.y = layout->y_coordinates[i];
            inputs.pressed = pressed[i];
            inputs.press = press[i];
            err = lumiscript_run_render(&inputs, i, &output);
            if (err) {
                LOG_ERR("Lumiscript render: %d", err);
                goto cont;
            }
            buf_back[layout->led_map[i]].r =
                apply_brightness((output.color >> 16) & 0xFF);
            buf_back[layout->led_map[i]].g =
                apply_brightness((output.color >> 8) & 0xFF);
            buf_back[layout->led_map[i]].b =
                apply_brightness(output.color & 0xFF);
        }
        struct led_rgb *tmp = buf_front;
        buf_front = buf_back;
        buf_back = tmp;
        err = led_strip_update_rgb(led_strip, buf_front, led_count);
        if (err) {
            LOG_ERR("led_strip_update_rgb: %d", err);
        }

    cont:
        k_mutex_unlock(&ykb_bl_mut);

        k_sleep(K_MSEC(thread_sleep_time));
    }
}

static void kscan_on_event(uint16_t index, bool value) {
    if (!layout || index >= layout->key_count) {
        return;
    }

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    pressed[index] = value;
    k_mutex_unlock(&ykb_bl_mut);
}

static void kscan_on_value_changed(uint16_t index, uint16_t value) {
    if (!layout || index >= layout->key_count) {
        return;
    }

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    press[index] = value;
    k_mutex_unlock(&ykb_bl_mut);
}

KSCAN_CB_DEFINE(ykb_backlight) = {
    .on_event = kscan_on_event,
    .on_new_value = kscan_on_value_changed,
};

static bool init_success = false;

static void on_settings_update(const kb_settings_t *settings) {
    const ykb_backlight_script_slot_t *slot;
    uint16_t cur_idx;
    int err;

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);

    if (!init_success) {
        k_mutex_unlock(&ykb_bl_mut);
        return;
    }

    clear_state();

    script_loaded = false;

    cur_speed = settings->backlight.speed;
    thread_sleep_time = settings->backlight.thread_sleep_ms;
    cur_brightness = settings->backlight.brightness;
    on = settings->backlight.on;

    cur_idx = settings->backlight.active_script_index;
    if (cur_idx >= ARRAY_SIZE(script_slots.slots)) {
        LOG_ERR("Active script index is out of bounds (%u >= %u)", cur_idx,
                (unsigned int)ARRAY_SIZE(script_slots.slots));
        goto defer;
    }

    slot = &script_slots.slots[cur_idx];
    if (!slot->occupied || slot->size == 0) {
        LOG_WRN("Backlight slot %u is empty", cur_idx);
        goto defer;
    }

    LOG_INF("Loading lumiscript slot %u '%s'", cur_idx, slot->name);
    err = lumiscript_load(slot->bytecode, slot->size);
    if (err) {
        LOG_ERR("Unable to load lumiscript '%s' (%d)", slot->name, err);
        goto defer;
    }

    err = lumiscript_run_init();
    if (err) {
        LOG_ERR("Unable to run lumiscript '%s' init (%d)", slot->name, err);
        goto defer;
    }

    script_loaded = true;
    LOG_INF("Successfully loaded lumiscript slot %u '%s'", cur_idx, slot->name);

defer:
    k_mutex_unlock(&ykb_bl_mut);
}

ON_SETTINGS_UPDATE_DEFINE(ykb_backlight, on_settings_update);

static int ykb_backlight_init(void) {
    int err;

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);

    if (!device_is_ready(led_strip)) {
        LOG_ERR("LED strip is not ready");
        k_mutex_unlock(&ykb_bl_mut);
        return -1;
    }
    layout = ykb_backlight_get_layout();
    if (!layout) {
        LOG_ERR("Backlight layout is not available");
        k_mutex_unlock(&ykb_bl_mut);
        return -1;
    }
    if (!layout->led_map || !layout->x_coordinates || !layout->y_coordinates) {
        LOG_ERR("Backlight layout is incomplete");
        k_mutex_unlock(&ykb_bl_mut);
        return -1;
    }
    if (layout->key_count != local_key_capacity) {
        LOG_ERR("Backlight layout key count mismatch (%u != %u)",
                (unsigned int)layout->key_count,
                (unsigned int)local_key_capacity);
        k_mutex_unlock(&ykb_bl_mut);
        return -1;
    }

    for (uint16_t i = 0; i < layout->key_count; ++i) {
        if (layout->x_coordinates[i] > 1000) {
            LOG_ERR("X coordinate %d is not in the range of [0-1000]", i);
            k_mutex_unlock(&ykb_bl_mut);
            return -1;
        }
        if (layout->y_coordinates[i] > 1000) {
            LOG_ERR("Y coordinate %d is not in the range of [0-1000]", i);
            k_mutex_unlock(&ykb_bl_mut);
            return -1;
        }
        if (layout->led_map[i] >= led_count) {
            LOG_ERR("LED map index %d points past chain length", i);
            k_mutex_unlock(&ykb_bl_mut);
            return -1;
        }
    }

    if (!scripts_registered) {
        err = settings_subsys_init();
        if (err) {
            LOG_ERR("settings_subsys_init: %d", err);
            k_mutex_unlock(&ykb_bl_mut);
            return err;
        }

        err = settings_register(&ykb_backlight_script_handler);
        if (err) {
            LOG_ERR("settings_register(backlight): %d", err);
            k_mutex_unlock(&ykb_bl_mut);
            return err;
        }

        scripts_registered = true;
    }

    scripts_loaded = false;
    memset(&script_slots, 0, sizeof(script_slots));
    err = settings_load_subtree(YKB_BACKLIGHT_SETTINGS_NS);
    if (err || !scripts_loaded) {
        LOG_WRN("No valid backlight script storage found (err %d). Loading "
                "defaults.",
                err);
        err = ykb_backlight_load_default_scripts();
        if (err) {
            LOG_ERR("ykb_backlight_load_default_scripts: %d", err);
            k_mutex_unlock(&ykb_bl_mut);
            return err;
        }
        ykb_backlight_save_scripts();
    }

    k_thread_create(&ykb_backlight_thread, ykb_backlight_thread_stack,
                    K_THREAD_STACK_SIZEOF(ykb_backlight_thread_stack),
                    ykb_backlight_thread_handler, NULL, NULL, NULL,
                    CONFIG_YKB_BL_THREAD_PRIORITY, 0, K_NO_WAIT);

    init_success = true;

    LOG_INF("YKB Backlight init ok.");

    k_mutex_unlock(&ykb_bl_mut);

    return 0;
}

SYS_INIT(ykb_backlight_init, POST_KERNEL, CONFIG_YKB_BL_INIT_PRIORITY);

const ykb_backlight_settings_t *ykb_backlight_get_default_settings() {
    return &default_backlight_settings;
}

const ykb_backlight_script_slots_t *
ykb_backlight_get_default_script_slots(void) {
    return default_script_slots;
}

size_t ykb_backlight_get_script_slot_count(void) {
    return ARRAY_SIZE(script_slots.slots);
}

int ykb_backlight_get_script_slot(uint16_t slot,
                                  ykb_backlight_script_slot_t *out) {
    if (!out) {
        return -EINVAL;
    }

    if (slot >= ARRAY_SIZE(script_slots.slots)) {
        return -ERANGE;
    }

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    memcpy(out, &script_slots.slots[slot], sizeof(*out));
    k_mutex_unlock(&ykb_bl_mut);

    return 0;
}

int ykb_backlight_get_script_slot_crc32(uint16_t slot, uint32_t *out_crc32) {
    ykb_backlight_script_slot_t slot_data;
    uint32_t crc = 0;
    size_t name_len;

    if (!out_crc32) {
        return -EINVAL;
    }

    if (slot >= ARRAY_SIZE(script_slots.slots)) {
        return -ERANGE;
    }

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    memcpy(&slot_data, &script_slots.slots[slot], sizeof(slot_data));
    k_mutex_unlock(&ykb_bl_mut);

    crc = crc32_ieee_update(crc, (const uint8_t *)&slot_data.occupied,
                            sizeof(slot_data.occupied));
    crc = crc32_ieee_update(crc, (const uint8_t *)&slot_data.size,
                            sizeof(slot_data.size));

    name_len = strnlen(slot_data.name, sizeof(slot_data.name));
    crc = crc32_ieee_update(crc, (const uint8_t *)slot_data.name, name_len);

    if (slot_data.occupied && slot_data.size > 0) {
        crc = crc32_ieee_update(crc, slot_data.bytecode, slot_data.size);
    }

    *out_crc32 = crc;
    return 0;
}

int ykb_backlight_set_script_slot(uint16_t slot,
                                  const ykb_backlight_script_slot_t *in) {
    kb_settings_t settings_snapshot;
    bool reload_active = false;

    if (!in) {
        return -EINVAL;
    }

    if (slot >= ARRAY_SIZE(script_slots.slots)) {
        return -ERANGE;
    }

    if (in->size > CONFIG_YKB_BL_SCRIPT_SLOT_SIZE) {
        return -EMSGSIZE;
    }

    if (kb_settings_get(&settings_snapshot) == 0) {
        reload_active = settings_snapshot.backlight.active_script_index == slot;
    }

    k_mutex_lock(&ykb_bl_mut, K_FOREVER);
    memcpy(&script_slots.slots[slot], in, sizeof(*in));
    k_mutex_unlock(&ykb_bl_mut);

    ykb_backlight_save_scripts();

    if (reload_active) {
        on_settings_update(&settings_snapshot);
    }

    if (script_slot_update_cb) {
        script_slot_update_cb(slot);
    }

    return 0;
}
