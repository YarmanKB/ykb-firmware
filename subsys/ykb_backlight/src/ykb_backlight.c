#include <subsys/ykb_backlight.h>

#include "generated_backlight_resources.h"
#include "lumiscript_vm.h"

#include <subsys/kb_settings.h>
#include <subsys/zephyr_user_helpers.h>

#include <drivers/kscan.h>

#include <math.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ykb_backlight, CONFIG_YKB_BACKLIGHT_LOG_LEVEL);

static const struct device *led_strip = Z_USER_DEV(ykb_backlight);
static const ykb_backlight_layout_t *layout;

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

    uint16_t cur_idx = settings->backlight.active_script_index;
    if (cur_idx >= settings->backlight.script_amount) {
        LOG_ERR("Active script index is out of bounds (%d >= %d)!", cur_idx,
                settings->backlight.script_amount);
        goto defer;
    }
    uint32_t start_offset = settings->backlight.offsets[cur_idx];
    uint32_t end_offset = settings->backlight.offsets[cur_idx + 1];

    const char *script_name = settings->backlight.names[cur_idx];
    LOG_INF("Loading lumiscript '%s'", script_name);
    int err = lumiscript_load(&settings->backlight.backlight_data[start_offset],
                              end_offset - start_offset);
    if (err) {
        LOG_ERR("Unable to load lumiscript '%s' (%d)", script_name, err);
        goto defer;
    }

    err = lumiscript_run_init();
    if (err) {
        LOG_ERR("Unable to run lumiscript '%s' init (%d)", script_name, err);
        goto defer;
    }

    script_loaded = true;
    LOG_INF("Successfully loaded lumiscript '%s'", script_name);

defer:
    k_mutex_unlock(&ykb_bl_mut);
}

ON_SETTINGS_UPDATE_DEFINE(ykb_backlight, on_settings_update);

static int ykb_backlight_init(void) {

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
