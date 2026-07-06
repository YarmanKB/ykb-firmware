#include "kb_handler_private.h"

#include <dt-bindings/kb-handler/kb-key-codes.h>
#include <subsys/ykb_metrics.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <math.h>
#include <string.h>

LOG_MODULE_REGISTER(kb_handler, CONFIG_KB_HANDLER_LOG_LEVEL);

static K_THREAD_STACK_DEFINE(kbh_core_thread_stack,
                             CONFIG_KB_HANDLER_THREAD_STACK_SIZE);
static struct k_thread kbh_core_thread;

static kb_settings_t settings_snapshot;
static bool thread_started;
static uint16_t values[TOTAL_KEY_COUNT];

static inline void notify_transition(uint16_t key, bool pressed) {
    STRUCT_SECTION_FOREACH(kb_handler_cb, callbacks) {
        if (callbacks->on_event) {
            callbacks->on_event(key, pressed);
        }
    }
}

static inline bool is_modifier(uint8_t hid) {
    return (hid >= KEY_LEFTCONTROL && hid <= KEY_RIGHTGUI);
}

static inline uint8_t mod_bit(uint8_t hid) {
    return 1U << (hid - KEY_LEFTCONTROL);
}

static inline uint8_t resolve_hid(const kb_settings_t *settings, uint16_t key,
                                  bool second_layer_active,
                                  bool third_layer_active) {
    if (third_layer_active) {
        return settings->mappings_layer3[key];
    }
    if (second_layer_active) {
        return settings->mappings_layer2[key];
    }

    return settings->mappings_layer1[key];
}

static inline bool add_key(uint8_t keys[6], uint8_t hid) {
    for (int i = 0; i < 6; ++i) {
        if (keys[i] == hid) {
            return true;
        }
    }

    for (int i = 0; i < 6; ++i) {
        if (keys[i] == 0U) {
            keys[i] = hid;
            return true;
        }
    }

    return false;
}

static bool is_mouseemu_physical_key(const kb_mouseemu_settings_t *emu,
                                     uint16_t key_index) {
    if (!emu || !emu->enabled) {
        return false;
    }

    for (uint8_t i = 0; i < emu->move_keys_count; ++i) {
        if (emu->move_keys[i] == key_index) {
            return true;
        }
    }

    for (uint8_t i = 0; i < emu->scroll_keys_count; ++i) {
        if (emu->scroll_keys[i] == key_index) {
            return true;
        }
    }

    for (uint8_t i = 0; i < emu->button_keys_count; ++i) {
        if (emu->button_keys[i] == key_index) {
            return true;
        }
    }

    return false;
}

static void build_kb_report(hid_kb_report_t *report, kb_settings_t *settings,
                            const bool *pressed_keys, uint16_t total_key_count,
                            bool second_layer_active, bool third_layer_active) {
    bool overflow = false;

    report->mods = 0;
    report->reserved = 0;
    memset(report->keys, 0, sizeof(report->keys));

    for (uint16_t i = 0; i < total_key_count; ++i) {
        uint8_t hid;

        if (!pressed_keys[i]) {
            continue;
        }

        if (settings->mode == KB_MODE_MOUSESIM &&
            is_mouseemu_physical_key(&settings->mouseemu, i)) {
            continue;
        }

        hid = resolve_hid(settings, i, second_layer_active, third_layer_active);

        if (hid == KEY_LAYER1 || hid == KEY_LAYER2 || hid == KEY_FN ||
            hid == KEY_NOKEY) {
            continue;
        }

        if (is_modifier(hid)) {
            report->mods |= mod_bit(hid);
            continue;
        }

        if (!add_key(report->keys, hid)) {
            overflow = true;
        }
    }

    if (overflow && IS_ENABLED(CONFIG_KB_HANDLER_REPORT_ROLLOVER)) {
        memset(report->keys, 0x01, sizeof(report->keys));
    }
}

static void build_layer_keys(const kb_settings_t *settings,
                             uint16_t total_key_count, uint16_t *layer1_keys,
                             uint8_t *layer1_keys_count, uint16_t *layer2_keys,
                             uint8_t *layer2_keys_count, uint16_t *fn_keys,
                             uint8_t *fn_keys_count) {
    *layer1_keys_count = 0;
    *layer2_keys_count = 0;
    *fn_keys_count = 0;

    memset(layer1_keys, 0, total_key_count * sizeof(uint16_t));
    memset(layer2_keys, 0, total_key_count * sizeof(uint16_t));
    memset(fn_keys, 0, total_key_count * sizeof(uint16_t));

    for (uint16_t i = 0; i < total_key_count; ++i) {
        if (settings->mappings_layer1[i] == KEY_LAYER1) {
            layer1_keys[(*layer1_keys_count)++] = i;
        }
        if (settings->mappings_layer1[i] == KEY_LAYER2) {
            layer2_keys[(*layer2_keys_count)++] = i;
        }
        if (settings->mappings_layer1[i] == KEY_FN) {
            fn_keys[(*fn_keys_count)++] = i;
        }
    }
}

static bool is_any_fn_pressed(const struct kbh_runtime_state *st,
                              uint16_t key) {
    for (uint8_t i = 0; i < st->fn_keys_count; ++i) {
        uint16_t fn_key = st->fn_keys[i];

        if (fn_key == key) {
            continue;
        }

        if (st->pressed_keys[fn_key] ||
            values[fn_key] >= st->settings->thresholds[fn_key]) {
            return true;
        }
    }

    return false;
}

static bool key_in_list(uint16_t key, const uint16_t *keys, uint8_t key_count) {
    for (uint8_t i = 0; i < key_count; ++i) {
        if (keys[i] == key) {
            return true;
        }
    }

    return false;
}

static void mutate_mode_normal(kb_settings_t *settings, void *user_data) {
    settings->mode = KB_MODE_NORMAL;
}

static void mutate_mode_race(kb_settings_t *settings, void *user_data) {
    settings->mode = KB_MODE_RACE;
}

static void mutate_mode_mousesim(kb_settings_t *settings, void *user_data) {
    settings->mode = KB_MODE_MOUSESIM;
}

static void mutate_transport_usb(kb_settings_t *settings, void *user_data) {
    settings->kbh_prio = KBH_TRANSPORT_PRIO_USB;
}

static void mutate_transport_bt(kb_settings_t *settings, void *user_data) {
    settings->kbh_prio = KBH_TRANSPORT_PRIO_BT;
}

#if CONFIG_YKB_BACKLIGHT
static bool find_next_occupied_script_slot(uint16_t current, int step,
                                           uint16_t *next_slot) {
    int32_t count = (int32_t)ykb_backlight_get_script_slot_count();
    ykb_backlight_script_slot_t slot;

    if (!next_slot || count <= 0) {
        return false;
    }

    for (int32_t attempt = 0; attempt < count; ++attempt) {
        int32_t idx =
            ((int32_t)current + count + ((attempt + 1) * step)) % count;

        if (ykb_backlight_get_script_slot((uint16_t)idx, &slot) == 0 &&
            slot.occupied && slot.size > 0U) {
            *next_slot = (uint16_t)idx;
            return true;
        }
    }

    return false;
}

static void mutate_bl_toggle(kb_settings_t *settings, void *user_data) {
    settings->backlight.on = !settings->backlight.on;
}

static void mutate_bl_next_script(kb_settings_t *settings, void *user_data) {
    uint16_t next_slot;

    if (find_next_occupied_script_slot(settings->backlight.active_script_index,
                                       1, &next_slot)) {
        settings->backlight.active_script_index = next_slot;
    }
}

static void mutate_bl_prev_script(kb_settings_t *settings, void *user_data) {
    uint16_t prev_slot;

    if (find_next_occupied_script_slot(settings->backlight.active_script_index,
                                       -1, &prev_slot)) {
        settings->backlight.active_script_index = prev_slot;
    }
}

static void mutate_bl_brightness_up(kb_settings_t *settings, void *user_data) {
    settings->backlight.brightness =
        MIN(settings->backlight.brightness + 0.1f, 1.0f);
}

static void mutate_bl_brightness_down(kb_settings_t *settings,
                                      void *user_data) {
    settings->backlight.brightness =
        MAX(settings->backlight.brightness - 0.1f, 0.0f);
}

static void mutate_bl_speed_up(kb_settings_t *settings, void *user_data) {
    settings->backlight.speed = MIN(settings->backlight.speed + 0.1f, 4.0f);
}

static void mutate_bl_speed_down(kb_settings_t *settings, void *user_data) {
    settings->backlight.speed = MAX(settings->backlight.speed - 0.1f, 0.1f);
}

static void mutate_bl_set_script(kb_settings_t *settings, void *user_data) {
    intptr_t slot = (intptr_t)user_data;
    size_t count = ykb_backlight_get_script_slot_count();

    if (count == 0U || slot < 0 || (size_t)slot >= count) {
        return;
    }

    settings->backlight.active_script_index = (uint16_t)slot;
}

static void mutate_bl_set_brightness(kb_settings_t *settings, void *user_data) {
    intptr_t percent = (intptr_t)user_data;

    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    settings->backlight.brightness = ((float)percent) / 100.0f;
}

static void mutate_bl_set_speed(kb_settings_t *settings, void *user_data) {
    intptr_t centi_speed = (intptr_t)user_data;

    if (centi_speed < 10) {
        centi_speed = 10;
    }
    if (centi_speed > 400) {
        centi_speed = 400;
    }

    settings->backlight.speed = ((float)centi_speed) / 100.0f;
}
#endif // CONFIG_YKB_BACKLIGHT

static bool execute_fn_action(const kb_fn_shortcut_t *shortcut,
                              kb_settings_t *settings) {
    intptr_t param = shortcut->param;

    switch (shortcut->action) {
    case KB_FN_ACTION_MODE_NORMAL:
        mutate_mode_normal(settings, NULL);
        return true;
    case KB_FN_ACTION_MODE_RACE:
        mutate_mode_race(settings, NULL);
        return true;
    case KB_FN_ACTION_MODE_MOUSESIM:
        mutate_mode_mousesim(settings, NULL);
        return true;
    case KB_FN_ACTION_TRANSPORT_USB:
        mutate_transport_usb(settings, NULL);
        return true;
    case KB_FN_ACTION_TRANSPORT_BT:
        mutate_transport_bt(settings, NULL);
        return true;
#if CONFIG_YKB_BACKLIGHT
    case KB_FN_ACTION_BL_TOGGLE:
        mutate_bl_toggle(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_NEXT_SCRIPT:
        mutate_bl_next_script(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_PREV_SCRIPT:
        mutate_bl_prev_script(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_BRIGHTNESS_UP:
        mutate_bl_brightness_up(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_BRIGHTNESS_DOWN:
        mutate_bl_brightness_down(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_SPEED_UP:
        mutate_bl_speed_up(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_SPEED_DOWN:
        mutate_bl_speed_down(settings, NULL);
        return true;
    case KB_FN_ACTION_BL_SET_SCRIPT:
        mutate_bl_set_script(settings, (void *)param);
        return true;
    case KB_FN_ACTION_BL_SET_BRIGHTNESS:
        mutate_bl_set_brightness(settings, (void *)param);
        return true;
    case KB_FN_ACTION_BL_SET_SPEED:
        mutate_bl_set_speed(settings, (void *)param);
        return true;
#endif // CONFIG_YKB_BACKLIGHT
    case KB_FN_ACTION_NONE:
    default:
        return false;
    }
}

static bool maybe_handle_fn_shortcut(struct kbh_runtime_state *st, uint16_t key,
                                     bool status, uint8_t resolved_hid) {
    bool fn_active = is_any_fn_pressed(st, key);

    if (st->shortcut_consumed_keys[key]) {
        if (!status) {
            st->shortcut_consumed_keys[key] = false;
        }
        return true;
    }

    if (!status || resolved_hid == KEY_FN || !fn_active) {
        return false;
    }

    for (size_t i = 0; i < st->settings->fn_shortcuts_count; ++i) {
        const kb_fn_shortcut_t *shortcut = &st->settings->fn_shortcuts[i];

        if (shortcut->key != resolved_hid) {
            continue;
        }

        if (execute_fn_action(shortcut, st->settings) &&
            kb_settings_apply(st->settings) == 0) {
            st->active_mode = st->settings->mode;
            st->shortcut_consumed_keys[key] = true;
            return true;
        }

        st->shortcut_consumed_keys[key] = true;
        return true;
    }

    st->shortcut_consumed_keys[key] = true;
    return true;
}

static bool handle_layer_key(uint16_t key, bool status,
                             const bool *pressed_keys, bool *layer_active,
                             const uint16_t *layer_keys,
                             uint8_t layer_key_count) {
    if (!status) {
        for (uint8_t i = 0; i < layer_key_count; ++i) {
            uint16_t layer_key = layer_keys[i];

            if (layer_key == key) {
                continue;
            }
            if (pressed_keys[layer_key]) {
                return false;
            }
        }
    }

    *layer_active = status;
    return true;
}

static inline bool kb_reports_equal(const hid_kb_report_t *a,
                                    const hid_kb_report_t *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static inline bool mouse_reports_equal(const hid_mouse_report_t *a,
                                       const hid_mouse_report_t *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static inline bool mouse_report_has_motion(const hid_mouse_report_t *report) {
    return report->x != 0 || report->y != 0 || report->wheel != 0;
}

#define KBH_MOUSEEMU_REPEAT_INTERVAL_MS 8

static inline int8_t clamp_s8(double v) {
    if (v > 127.0) {
        return 127;
    }
    if (v < -128.0) {
        return -128;
    }

    return (int8_t)lround(v);
}

#define KB_MOUSEEMU_MOVE_SCALE_MAX 100U
#define KB_MOUSEEMU_SCROLL_SCALE_MAX 8U

static inline uint16_t
key_value_from_minimum_range_scaled(uint16_t current_value, uint16_t minimum,
                                    uint16_t maximum, uint16_t deadzone,
                                    uint16_t scale_max) {
    uint32_t baseline = (uint32_t)minimum + (uint32_t)deadzone;
    uint32_t current;
    uint32_t span;
    uint32_t active;

    if (maximum <= minimum) {
        return 0;
    }

    if (baseline > maximum) {
        baseline = maximum;
    }

    if (current_value <= baseline) {
        return 0;
    }

    if (current_value > maximum) {
        current_value = maximum;
    }

    current = current_value;
    span = (uint32_t)maximum - baseline;
    if (span == 0U) {
        return 0;
    }

    active = current - baseline;

    return (uint16_t)((active * scale_max + (span / 2U)) / span);
}

static inline uint16_t key_value_from_minimum_range(uint16_t current_value,
                                                    uint16_t minimum,
                                                    uint16_t maximum,
                                                    uint16_t deadzone) {
    return key_value_from_minimum_range_scaled(
        current_value, minimum, maximum, deadzone, KB_MOUSEEMU_MOVE_SCALE_MAX);
}

static inline uint16_t key_press_percent(const kb_settings_t *settings,
                                         uint16_t key, uint16_t current_value) {
    if (key >= TOTAL_KEY_COUNT) {
        return 0;
    }

    return key_value_from_minimum_range_scaled(
        current_value, settings->minimums[key], settings->maximums[key],
        settings->deadzones[key], KB_HANDLER_PRESS_PERCENT_MAX);
}

static inline void notify_value_if_changed(struct kbh_runtime_state *st,
                                           uint16_t key,
                                           uint16_t press_percent) {
    if (key >= TOTAL_KEY_COUNT ||
        st->notified_press_percent[key] == press_percent) {
        return;
    }

    st->notified_press_percent[key] = press_percent;
    STRUCT_SECTION_FOREACH(kb_handler_cb, callbacks) {
        if (callbacks->on_new_value) {
            callbacks->on_new_value(key, press_percent);
        }
    }
}

static inline uint16_t
key_scroll_value_from_minimum_range(uint16_t current_value, uint16_t minimum,
                                    uint16_t maximum, uint16_t deadzone) {
    return key_value_from_minimum_range_scaled(current_value, minimum, maximum,
                                               deadzone,
                                               KB_MOUSEEMU_SCROLL_SCALE_MAX);
}

static inline double mouseemu_scroll_delta(kb_settings_t *settings,
                                           uint16_t *current_values,
                                           kb_mouseemu_settings_t *emu) {
    uint16_t up_idx = emu->scroll_keys[0];
    uint16_t down_idx = emu->scroll_keys[1];
    int32_t up = (int32_t)key_scroll_value_from_minimum_range(
        current_values[up_idx], settings->minimums[up_idx],
        settings->maximums[up_idx], settings->deadzones[up_idx]);
    int32_t down = (int32_t)key_scroll_value_from_minimum_range(
        current_values[down_idx], settings->minimums[down_idx],
        settings->maximums[down_idx], settings->deadzones[down_idx]);

    return (double)(up - down) * emu->scroll_k;
}

static inline uint16_t
mouseemu_vector_value(kb_settings_t *settings, uint16_t *current_values,
                      kb_mouseemu_settings_t *emu, uint8_t left_vec_idx,
                      uint8_t straight_vec_idx, uint8_t right_vec_idx) {
    uint16_t straight_key_idx = emu->move_keys[straight_vec_idx];
    uint16_t straight_vec = key_value_from_minimum_range(
        current_values[straight_key_idx], settings->minimums[straight_key_idx],
        settings->maximums[straight_key_idx],
        settings->deadzones[straight_key_idx]);

    if (emu->direction_mode == KB_MOUSEEMU_DIRECTION_8_WAY) {
        uint16_t left_key_idx = emu->move_keys[left_vec_idx];
        uint16_t left_vec = key_value_from_minimum_range(
            current_values[left_key_idx], settings->minimums[left_key_idx],
            settings->maximums[left_key_idx],
            settings->deadzones[left_key_idx]);
        uint16_t right_key_idx = emu->move_keys[right_vec_idx];
        uint16_t right_vec = key_value_from_minimum_range(
            current_values[right_key_idx], settings->minimums[right_key_idx],
            settings->maximums[right_key_idx],
            settings->deadzones[right_key_idx]);

        return straight_vec + (left_vec / 2U) + (right_vec / 2U);
    }

    return straight_vec;
}

static void mouseemu_value_handler(kb_settings_t *settings,
                                   uint16_t *current_values, bool *pressed_keys,
                                   hid_mouse_report_t *report,
                                   double *wheel_delta) {
    kb_mouseemu_settings_t *emu = &settings->mouseemu;

    report->buttons = 0;
    report->x = 0;
    report->y = 0;
    report->wheel = 0;
    *wheel_delta = 0.0;

    if (!emu->enabled) {
        return;
    }

    if (emu->move_keys_count > 0U) {
        int32_t y_pos = (int32_t)mouseemu_vector_value(settings, current_values,
                                                       emu, 4, 1, 5);
        int32_t y_neg = (int32_t)mouseemu_vector_value(settings, current_values,
                                                       emu, 7, 2, 6);
        int32_t x_pos = (int32_t)mouseemu_vector_value(settings, current_values,
                                                       emu, 5, 3, 7);
        int32_t x_neg = (int32_t)mouseemu_vector_value(settings, current_values,
                                                       emu, 6, 0, 4);

        report->x = clamp_s8((double)(x_pos - x_neg) * emu->move_x_k);
        report->y = clamp_s8((double)(y_neg - y_pos) * emu->move_y_k);
    }

    if (emu->scroll_keys_count == 2U) {
        *wheel_delta = mouseemu_scroll_delta(settings, current_values, emu);
    }

    if (emu->button_keys_count >= 1U && pressed_keys[emu->button_keys[0]]) {
        report->buttons |= BIT(0);
    }
    if (emu->button_keys_count >= 2U && pressed_keys[emu->button_keys[1]]) {
        report->buttons |= BIT(1);
    }
    if (emu->button_keys_count >= 3U && pressed_keys[emu->button_keys[2]]) {
        report->buttons |= BIT(2);
    }
}

static inline void send_kb_report_if_changed(struct kbh_runtime_state *st) {
    build_kb_report(&st->kb_report, st->settings, st->pressed_keys,
                    TOTAL_KEY_COUNT, st->second_layer_active,
                    st->third_layer_active);

    if (!kb_reports_equal(&st->kb_report, &st->prev_kb_report)) {
        kb_handler_transport_send_kb_report(&st->kb_report,
                                            st->settings->kbh_prio);
        YKB_METRICS_REPORT_SENT(YKB_METRICS_REPORT_KBD);
        st->prev_kb_report = st->kb_report;
    }
}

static inline void send_mouse_report_if_changed(struct kbh_runtime_state *st) {
    int64_t now_ms = k_uptime_get();
    double wheel_delta;
    double wheel_total;
    int32_t wheel_steps;
    bool report_changed;
    bool repeat_due;
    bool should_send;

    mouseemu_value_handler(st->settings, st->current_values, st->pressed_keys,
                           &st->mouse_report, &wheel_delta);

    wheel_total = st->mouse_wheel_remainder + wheel_delta;
    wheel_steps = (int32_t)wheel_total;
    if (wheel_steps > 127) {
        wheel_steps = 127;
    } else if (wheel_steps < -128) {
        wheel_steps = -128;
    }

    st->mouse_report.wheel = (int8_t)wheel_steps;
    st->mouse_wheel_remainder = wheel_total - (double)wheel_steps;

    report_changed =
        !mouse_reports_equal(&st->mouse_report, &st->prev_mouse_report);
    repeat_due =
        mouse_report_has_motion(&st->mouse_report) &&
        (now_ms - st->last_mouse_report_ms) >= KBH_MOUSEEMU_REPEAT_INTERVAL_MS;
    should_send = report_changed || repeat_due;

    if (should_send) {
        kb_handler_transport_send_mouse_report(&st->mouse_report,
                                               st->settings->kbh_prio);
        YKB_METRICS_REPORT_SENT(YKB_METRICS_REPORT_MOUSE);
        st->prev_mouse_report = st->mouse_report;
        st->last_mouse_report_ms = now_ms;
    }
}

static void send_race_report_if_changed(struct kbh_runtime_state *st) {
    double max_percentage = 0.0;
    int32_t max_index = -1;

    memset(st->race_pressed_keys, 0, sizeof(st->race_pressed_keys));

    for (uint16_t i = 0; i < TOTAL_KEY_COUNT; ++i) {
        uint16_t threshold;
        uint16_t maximum;
        double percentage_pressed;

        if (!st->pressed_keys[i]) {
            continue;
        }

        threshold = st->settings->thresholds[i];
        maximum = st->settings->maximums[i];
        if (maximum <= threshold) {
            continue;
        }

        percentage_pressed =
            (double)(st->current_values[i] - threshold) / (maximum - threshold);
        if (percentage_pressed > max_percentage) {
            max_percentage = percentage_pressed;
            max_index = i;
        }
    }

    if (max_index >= 0) {
        st->race_pressed_keys[max_index] = true;
    }

    build_kb_report(&st->kb_report, st->settings, st->race_pressed_keys,
                    TOTAL_KEY_COUNT, st->second_layer_active,
                    st->third_layer_active);

    if (!kb_reports_equal(&st->kb_report, &st->prev_kb_report)) {
        kb_handler_transport_send_kb_report(&st->kb_report,
                                            st->settings->kbh_prio);
        YKB_METRICS_REPORT_SENT(YKB_METRICS_REPORT_KBD);
        st->prev_kb_report = st->kb_report;
    }
}

static inline void reset_handler_state(struct kbh_runtime_state *st) {
    memset(st->pressed_keys, 0, sizeof(st->pressed_keys));
    memset(st->current_values, 0, sizeof(st->current_values));
    memset(st->notified_press_percent, 0, sizeof(st->notified_press_percent));
    memset(st->local_scan_values, 0, sizeof(st->local_scan_values));
    memset(st->race_pressed_keys, 0, sizeof(st->race_pressed_keys));
    memset(values, 0, sizeof(values));

    st->second_layer_active = false;
    st->third_layer_active = false;

    memset(&st->kb_report, 0, sizeof(st->kb_report));
    memset(&st->mouse_report, 0, sizeof(st->mouse_report));
    st->last_mouse_report_ms = 0;
    st->mouse_wheel_remainder = 0.0;

    kb_handler_transport_send_kb_report(&st->kb_report, st->settings->kbh_prio);
    YKB_METRICS_REPORT_SENT(YKB_METRICS_REPORT_KBD);
    kb_handler_transport_send_mouse_report(&st->mouse_report,
                                           st->settings->kbh_prio);
    YKB_METRICS_REPORT_SENT(YKB_METRICS_REPORT_MOUSE);

    st->prev_kb_report = st->kb_report;
    st->prev_mouse_report = st->mouse_report;
}

static inline void rebuild_layer_cache(struct kbh_runtime_state *st) {
    build_layer_keys(st->settings, TOTAL_KEY_COUNT, st->layer1_keys,
                     &st->layer1_keys_count, st->layer2_keys,
                     &st->layer2_keys_count, st->fn_keys, &st->fn_keys_count);
}

static void process_key_transition(struct kbh_runtime_state *st, uint16_t key,
                                   bool status) {
    uint8_t resolved_hid;
    bool is_layer1_key;
    bool is_layer2_key;
    bool is_fn_key;

    if (key >= TOTAL_KEY_COUNT) {
        LOG_WRN("Ignoring out-of-range key %u", key);
        return;
    }

    st->pressed_keys[key] = status;
    YKB_METRICS_KB_TRANSITION(status);
    notify_transition(key, status);

    is_layer1_key = key_in_list(key, st->layer1_keys, st->layer1_keys_count);
    is_layer2_key = key_in_list(key, st->layer2_keys, st->layer2_keys_count);
    is_fn_key = key_in_list(key, st->fn_keys, st->fn_keys_count);

    resolved_hid = resolve_hid(st->settings, key, st->second_layer_active,
                               st->third_layer_active);

    if (maybe_handle_fn_shortcut(st, key, status, resolved_hid)) {
        return;
    }

    if (st->active_mode == KB_MODE_RACE) {
        return;
    }

    if (is_layer1_key) {
        if (!handle_layer_key(key, status, st->pressed_keys,
                              &st->second_layer_active, st->layer1_keys,
                              st->layer1_keys_count)) {
            return;
        }
    } else if (is_layer2_key) {
        if (!handle_layer_key(key, status, st->pressed_keys,
                              &st->third_layer_active, st->layer2_keys,
                              st->layer2_keys_count)) {
            return;
        }
    } else if (is_fn_key) {
        return;
    }

    if (st->active_mode == KB_MODE_NORMAL ||
        st->active_mode == KB_MODE_MOUSESIM) {
        send_kb_report_if_changed(st);
    }
}

static bool process_key_value(struct kbh_runtime_state *st, uint16_t key,
                              uint16_t value) {
    bool was_pressed;
    bool pressed;

    if (key >= TOTAL_KEY_COUNT) {
        LOG_WRN("Ignoring out-of-range key value %u", key);
        return false;
    }

    was_pressed = st->pressed_keys[key];
    pressed = value >= st->settings->thresholds[key];

    st->current_values[key] = value;
    values[key] = value;
    notify_value_if_changed(st, key,
                            key_press_percent(st->settings, key, value));

    if (was_pressed == pressed) {
        return false;
    }

    process_key_transition(st, key, pressed);
    return true;
}

static void finish_value_frame(struct kbh_runtime_state *st) {
    if (st->active_mode == KB_MODE_MOUSESIM) {
        send_mouse_report_if_changed(st);
    }

    if (st->active_mode == KB_MODE_RACE) {
        send_race_report_if_changed(st);
    }
}

static bool scan_local_values(struct kbh_runtime_state *st) {
    size_t kscan_count = kb_handler_kscan_count();
    bool ok = true;

    if (kscan_count == 0U) {
        k_sleep(K_MSEC(1));
        return false;
    }

    for (size_t i = 0; i < kscan_count; ++i) {
        const struct device *kscan = kb_handler_get_kscan(i);
        int offset = kscan_get_idx_offset(kscan);
        int amount = kscan_get_key_amount(kscan);
        int err;

        if (offset < 0 || amount < 0 ||
            (uint32_t)offset + (uint32_t)amount > LOCAL_SCAN_KEY_COUNT) {
            LOG_ERR("Invalid kscan topology at runtime: offset=%d amount=%d",
                    offset, amount);
            ok = false;
            continue;
        }

        err = kscan_scan(kscan, &st->local_scan_values[offset]);
        if (err) {
            LOG_ERR("kscan_scan(%s): %d", kscan->name, err);
            ok = false;
        }
    }

    return ok;
}

static void process_local_scan_frame(struct kbh_runtime_state *st) {
    for (uint16_t i = 0U; i < LOCAL_SCAN_KEY_COUNT; ++i) {
        process_key_value(st, LOCAL_SCAN_GLOBAL_OFFSET + i,
                          st->local_scan_values[i]);
    }

    STRUCT_SECTION_FOREACH(kb_handler_core_hook, it) {
        if (it->on_local_scan_frame) {
            it->on_local_scan_frame(st);
        }
    }

    finish_value_frame(st);
}

static void kb_handler_thread(void *a, void *b, void *c) {
    struct kbh_runtime_state st = {
        .settings = &settings_snapshot,
        .active_mode = settings_snapshot.mode,
    };

    rebuild_layer_cache(&st);
    reset_handler_state(&st);

    while (true) {

        STRUCT_SECTION_FOREACH(kb_handler_core_hook, it) {
            if (it->on_thread_update) {
                it->on_thread_update(&st);
            }
        }

        if (scan_local_values(&st)) {
            process_local_scan_frame(&st);
        } else {
            k_sleep(K_USEC(1));
        }
    }
}

static void mouseemu_key_indices_check(const uint16_t *keys, uint8_t keys_count,
                                       uint16_t total_key_count,
                                       const char *group_name) {
    for (uint8_t i = 0; i < keys_count; ++i) {
        if (keys[i] >= total_key_count) {
            LOG_ERR("Mouseemu %s key index %u is out of range", group_name,
                    keys[i]);
            k_panic();
        }
    }
}

static void mouseemu_count_check(uint8_t count, uint8_t max_count,
                                 const char *group_name) {
    if (count > max_count) {
        LOG_ERR("Mouseemu %s count %u exceeds max %u", group_name, count,
                max_count);
        k_panic();
    }
}

static void mouseemu_check(uint16_t total_key_count,
                           const kb_mouseemu_settings_t *mouseemu) {
    if (!mouseemu->enabled) {
        return;
    }

    mouseemu_count_check(mouseemu->move_keys_count, KB_MOUSEEMU_MOVE_KEYS_MAX,
                         "move");
    mouseemu_count_check(mouseemu->scroll_keys_count,
                         KB_MOUSEEMU_SCROLL_KEYS_MAX, "scroll");
    mouseemu_count_check(mouseemu->button_keys_count,
                         KB_MOUSEEMU_BUTTON_KEYS_MAX, "button");

    if (mouseemu->direction_mode == KB_MOUSEEMU_DIRECTION_4_WAY &&
        mouseemu->move_keys_count != 4U) {
        LOG_ERR("Mouseemu 4-way mode requires exactly 4 move keys");
        k_panic();
    }

    if (mouseemu->direction_mode == KB_MOUSEEMU_DIRECTION_8_WAY &&
        mouseemu->move_keys_count != 8U) {
        LOG_ERR("Mouseemu 8-way mode requires exactly 8 move keys");
        k_panic();
    }

    if (mouseemu->scroll_keys_count != 0U &&
        mouseemu->scroll_keys_count != 2U) {
        LOG_ERR("Mouseemu scroll keys must contain exactly 2 entries");
        k_panic();
    }

    if (mouseemu->button_keys_count != 0U &&
        mouseemu->button_keys_count != 3U) {
        LOG_ERR("Mouseemu button keys must contain exactly 3 entries");
        k_panic();
    }

    mouseemu_key_indices_check(mouseemu->move_keys, mouseemu->move_keys_count,
                               total_key_count, "move");
    mouseemu_key_indices_check(mouseemu->scroll_keys,
                               mouseemu->scroll_keys_count, total_key_count,
                               "scroll");
    mouseemu_key_indices_check(mouseemu->button_keys,
                               mouseemu->button_keys_count, total_key_count,
                               "button");
}

static void kb_handler_on_settings_update(const kb_settings_t *settings) {
    bool from_core_thread =
        thread_started && (k_current_get() == &kbh_core_thread);

    if (thread_started && !from_core_thread) {
        k_thread_suspend(&kbh_core_thread);
        // k_msgq_purge(&kbh_core_msgq);
        // k_work_cancel_delayable(&slave_values_retry_work);
        // atomic_store_explicit(&slave_values_msg_pending, false,
        //                       memory_order_relaxed);
        // atomic_store_explicit(&slave_values_dirty, false,
        // memory_order_relaxed);
    }

    memcpy(&settings_snapshot, settings, sizeof(settings_snapshot));
    mouseemu_check(TOTAL_KEY_COUNT, &settings_snapshot.mouseemu);

    if (thread_started && !from_core_thread) {
        k_thread_resume(&kbh_core_thread);
    } else if (!thread_started) {
        k_thread_create(&kbh_core_thread, kbh_core_thread_stack,
                        CONFIG_KB_HANDLER_THREAD_STACK_SIZE, kb_handler_thread,
                        NULL, NULL, NULL, CONFIG_KB_HANDLER_THREAD_PRIORITY, 0,
                        K_NO_WAIT);
        k_thread_name_set(&kbh_core_thread, "kb_handler_core");
        thread_started = true;
    }
}

ON_SETTINGS_UPDATE_DEFINE(kbh_core, kb_handler_on_settings_update);

int kb_handler_core_init(void) {
    int err;

    thread_started = false;
    memset(values, 0, sizeof(values));

    err = kb_handler_check_kscans_ready();
    if (err) {
        return err;
    }

    err = kb_handler_validate_kscan_topology(LOCAL_SCAN_KEY_COUNT);
    if (err) {
        return err;
    }

    mouseemu_check(TOTAL_KEY_COUNT, &settings_snapshot.mouseemu);
    return 0;
}

void kb_handler_core_get_values(uint16_t *out_values, uint16_t count) {
    if (!out_values || count == 0U) {
        return;
    }

    if (count > TOTAL_KEY_COUNT) {
        count = TOTAL_KEY_COUNT;
    }

    memcpy(out_values, values, count * sizeof(uint16_t));
}
