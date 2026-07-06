#ifndef SUBSYS_KB_HANDLER_CORE_H
#define SUBSYS_KB_HANDLER_CORE_H

#include <subsys/kb_settings.h>
#include <subsys/usb_connect.h>
#include <subsys/zephyr_user_helpers.h>

#include <stdbool.h>
#include <stdint.h>

#define KEY_COUNT Z_USER_PROP(kb_handler_key_count)
#define KEY_COUNT_SLAVE Z_USER_PROP_OR(kb_handler_key_count_slave, 0)

#define KBH_SLAVE_VALUES_CAPACITY                                              \
    ((KEY_COUNT_SLAVE > 0U) ? KEY_COUNT_SLAVE : 1U)

#if CONFIG_KB_HANDLER_SPLITLINK_SLAVE
#define LOCAL_SCAN_KEY_COUNT KEY_COUNT_SLAVE
#define LOCAL_SCAN_GLOBAL_OFFSET KEY_COUNT
#else
#define LOCAL_SCAN_KEY_COUNT KEY_COUNT
#define LOCAL_SCAN_GLOBAL_OFFSET 0U
#endif // CONFIG_KB_HANDLER_SPLITLINK_SLAVE

#define KBH_LOCAL_SCAN_CAPACITY                                                \
    ((LOCAL_SCAN_KEY_COUNT > 0U) ? LOCAL_SCAN_KEY_COUNT : 1U)

struct kbh_runtime_state {
    kb_settings_t *settings;
    kb_mode_t active_mode;

    bool second_layer_active;
    bool third_layer_active;

    bool pressed_keys[TOTAL_KEY_COUNT];
    uint16_t current_values[TOTAL_KEY_COUNT];
    uint16_t notified_press_percent[TOTAL_KEY_COUNT];
    uint16_t local_scan_values[KBH_LOCAL_SCAN_CAPACITY];
    bool race_pressed_keys[TOTAL_KEY_COUNT];

    uint16_t layer1_keys[TOTAL_KEY_COUNT];
    uint16_t layer2_keys[TOTAL_KEY_COUNT];
    uint16_t fn_keys[TOTAL_KEY_COUNT];
    uint8_t layer1_keys_count;
    uint8_t layer2_keys_count;
    uint8_t fn_keys_count;
    bool shortcut_consumed_keys[TOTAL_KEY_COUNT];

    hid_kb_report_t kb_report;
    hid_kb_report_t prev_kb_report;
    hid_mouse_report_t mouse_report;
    hid_mouse_report_t prev_mouse_report;
    int64_t last_mouse_report_ms;
    double mouse_wheel_remainder;
};

// API mainly for splitlink sync glue

int kb_handler_core_init(void);

void kb_handler_core_get_values(uint16_t *out_values, uint16_t count);

void kb_handler_core_handle_slave_values(const uint16_t *values,
                                         uint16_t count);
void kb_handler_core_handle_slave_reset(void);

struct kb_handler_core_hook {
    void (*on_thread_update)(struct kbh_runtime_state *st);
    void (*on_local_scan_frame)(struct kbh_runtime_state *st);
};

#define KB_HANDLER_CORE_HOOK_DEFINE(name)                                      \
    static STRUCT_SECTION_ITERABLE(kb_handler_core_hook,                       \
                                   kb_handler_core_hook_##name)

#endif // SUBSYS_KB_HANDLER_CORE_H
