#ifndef YKB_FEATURES_H
#define YKB_FEATURES_H

#include <subsys/ykb_backlight.h>
#include <subsys/zephyr_user_helpers.h>

#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>

#include <stdbool.h>
#include <stdint.h>

#define FEATURES_MAX_BOARD_NAME 10
#define FEATURES_MAX_REV_NAME 5
#define FEATURES_MAX_VENDOR_NAME 10
#define FEATURES_MAX_SOC_NAME 10

#define FEATURES_VERSION_1 1U
#define FEATURES_VERSION FEATURES_VERSION_1

typedef struct __packed {
    const uint8_t features_version;

    const char board_name[FEATURES_MAX_BOARD_NAME];
    const char rev_name[FEATURES_MAX_REV_NAME];
    const char vendor_name[FEATURES_MAX_VENDOR_NAME];
    const char soc_name[FEATURES_MAX_SOC_NAME];

    const uint16_t key_count;
    const uint16_t key_count_slave;

    const uint8_t ykb_backlight_max_brightness_percent;
    const uint16_t ykb_backlight_const_cap;
    const uint16_t ykb_backlight_global_cap;
    const uint16_t ykb_backlight_key_var_cap;
    const uint16_t ykb_backlight_code_cap;
    const uint16_t ykb_backlight_stack_cap;
    const uint16_t ykb_backlight_script_slot_count;
    const uint16_t ykb_backlight_script_slot_size;
    const uint16_t ykb_backlight_script_name_max_len;
    const uint16_t kb_fn_shortcuts_max;

    const bool splitlink : 1;
    const bool splitlink_sync : 1;
    const bool ykb_backlight : 1;
    const bool ykb_battsense : 1;
    const bool ykb_battsense_pw_cutoff_present : 1;
    const bool ykb_battsense_pw_cutoff_on_failure : 1;
    const bool bt_connect_kbd : 1;
    const bool bt_connect_mouse : 1;
    const bool bt_connect_vendor : 1;
    const bool usb_connect_kbd : 1;
    const bool usb_connect_mouse : 1;
    const bool usb_connect_vendor : 1;
    const bool ykb_power : 1;
    const bool ykb_power_indicator : 1;
    const bool ykb_power_indicator_brightness_support : 1;
    const bool ykb_power_button : 1;
    const bool ykb_power_shutdown_gpios : 1;

} device_features;

#define FEATURE(name, config) .name = IS_ENABLED(config)

#define FEATURE_DEP(name, dep, value)                                          \
    .name = COND_CODE_1(IS_ENABLED(dep), (value), (0))

#define FEATURES_DEFINE(name)                                                  \
    device_features name = {                                                   \
        .features_version = FEATURES_VERSION,                                  \
        .board_name = CONFIG_BOARD,                                            \
        .rev_name = CONFIG_BOARD_REVISION,                                     \
        .vendor_name = "YarmanKB",                                             \
        .soc_name = CONFIG_SOC,                                                \
                                                                               \
        .key_count = CONFIG_KB_SETTINGS_KEY_COUNT,                             \
        FEATURE_DEP(key_count_slave, CONFIG_SPLITLINK_SYNC,                    \
                    CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE),                       \
        .kb_fn_shortcuts_max = CONFIG_KB_SETTINGS_FN_SHORTCUTS_MAX,            \
                                                                               \
        FEATURE(splitlink, CONFIG_SPLITLINK),                                  \
        FEATURE(splitlink_sync, CONFIG_SPLITLINK_SYNC_MASTER),                 \
                                                                               \
        FEATURE(ykb_backlight, CONFIG_YKB_BACKLIGHT),                          \
        FEATURE_DEP(ykb_backlight_max_brightness_percent,                      \
                    CONFIG_YKB_BACKLIGHT,                                      \
                    YKB_BACKLIGHT_MAX_ABS_BRIGHTNESS_PERCENT),                 \
        FEATURE_DEP(ykb_backlight_const_cap, CONFIG_YKB_BACKLIGHT,             \
                    CONFIG_YKB_BL_LUMIVM_CONST_CAPACITY),                      \
        FEATURE_DEP(ykb_backlight_key_var_cap, CONFIG_YKB_BACKLIGHT,           \
                    CONFIG_YKB_BL_LUMIVM_KEY_VAR_CAPACITY),                    \
        FEATURE_DEP(ykb_backlight_global_cap, CONFIG_YKB_BACKLIGHT,            \
                    CONFIG_YKB_BL_LUMIVM_GLOBAL_CAPACITY),                     \
        FEATURE_DEP(ykb_backlight_code_cap, CONFIG_YKB_BACKLIGHT,              \
                    CONFIG_YKB_BL_LUMIVM_CODE_CAPACITY),                       \
        FEATURE_DEP(ykb_backlight_stack_cap, CONFIG_YKB_BACKLIGHT,             \
                    CONFIG_YKB_BL_LUMIVM_STACK_CAPACITY),                      \
        FEATURE_DEP(ykb_backlight_script_slot_count, CONFIG_YKB_BACKLIGHT,     \
                    CONFIG_YKB_BL_SCRIPT_SLOT_COUNT),                          \
        FEATURE_DEP(ykb_backlight_script_slot_size, CONFIG_YKB_BACKLIGHT,      \
                    CONFIG_YKB_BL_SCRIPT_SLOT_SIZE),                           \
        FEATURE_DEP(ykb_backlight_script_name_max_len, CONFIG_YKB_BACKLIGHT,   \
                    CONFIG_YKB_BL_SCRIPT_NAME_MAX_LEN),                        \
                                                                               \
        FEATURE(ykb_battsense, CONFIG_YKB_BATTSENSE),                          \
        FEATURE(ykb_battsense_pw_cutoff_present,                               \
                CONFIG_YKB_BATTSENSE_PW_CUTOFF_PRESENT),                       \
        FEATURE(ykb_battsense_pw_cutoff_on_failure,                            \
                CONFIG_YKB_BATTSENSE_SHUTOFF_ON_CHARGER_FAILURE),              \
                                                                               \
        FEATURE(bt_connect_kbd, CONFIG_BT_CONNECT_KBD),                        \
        FEATURE(bt_connect_mouse, CONFIG_BT_CONNECT_MOUSE),                    \
        FEATURE(bt_connect_vendor, CONFIG_BT_CONNECT_VENDOR),                  \
                                                                               \
        FEATURE(usb_connect_kbd, CONFIG_USB_CONNECT_KBD),                      \
        FEATURE(usb_connect_mouse, CONFIG_USB_CONNECT_MOUSE),                  \
        FEATURE(usb_connect_vendor, CONFIG_USB_CONNECT_VENDOR),                \
                                                                               \
        FEATURE(ykb_power, CONFIG_YKB_POWER),                                  \
        FEATURE(ykb_power_indicator, CONFIG_YKB_POWER_INDICATOR_PRESENT),      \
        FEATURE(ykb_power_indicator_brightness_support,                        \
                CONFIG_YKB_POWER_INDICATOR_BRIGHTNESS_SUPPORT),                \
        FEATURE(ykb_power_button, CONFIG_YKB_POWER_BUTTON_PRESENT),            \
        FEATURE(ykb_power_shutdown_gpios,                                      \
                CONFIG_YKB_POWER_SHUTDOWN_GPIOS_PRESENT),                      \
    }

#endif // YKB_FEATURES_H
