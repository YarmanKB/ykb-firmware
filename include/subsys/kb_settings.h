#ifndef KB_SETTINGS_H_
#define KB_SETTINGS_H_

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>

#include <stdbool.h>
#include <stdint.h>

#if CONFIG_YKB_BACKLIGHT
#include <subsys/ykb_backlight.h>
#endif // CONFIG_YKB_BACKLIGHT

#if CONFIG_YKB_BATTSENSE
#include <subsys/ykb_battsense.h>
#endif // CONFIG_YKB_BATTSENSE

#if CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE
#define TOTAL_KEY_COUNT                                                        \
    (CONFIG_KB_SETTINGS_KEY_COUNT + CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE)
#else
#define TOTAL_KEY_COUNT CONFIG_KB_SETTINGS_KEY_COUNT
#endif // CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE

typedef enum {
    KB_MODE_NORMAL = 0U,
    KB_MODE_RACE = 1U,
    KB_MODE_MOUSESIM = 2U,
} kb_mode_t;

typedef enum {
    KB_MOUSEEMU_DIRECTION_4_WAY = 0U,
    KB_MOUSEEMU_DIRECTION_8_WAY = 1U,
} kb_mouseemu_direction_t;

#define KB_MOUSEEMU_MOVE_KEYS_MAX 8U
#define KB_MOUSEEMU_SCROLL_KEYS_MAX 2U
#define KB_MOUSEEMU_BUTTON_KEYS_MAX 3U

enum kb_handler_transport_priority {
    KBH_TRANSPORT_PRIO_USB = 0U,
    KBH_TRANSPORT_PRIO_BT = 1U,
};

typedef enum {
    KB_FN_ACTION_NONE = 0U,
    KB_FN_ACTION_MODE_NORMAL,
    KB_FN_ACTION_MODE_RACE,
    KB_FN_ACTION_MODE_MOUSESIM,
    KB_FN_ACTION_TRANSPORT_USB,
    KB_FN_ACTION_TRANSPORT_BT,
    KB_FN_ACTION_BL_TOGGLE,
    KB_FN_ACTION_BL_NEXT_SCRIPT,
    KB_FN_ACTION_BL_PREV_SCRIPT,
    KB_FN_ACTION_BL_BRIGHTNESS_UP,
    KB_FN_ACTION_BL_BRIGHTNESS_DOWN,
    KB_FN_ACTION_BL_SPEED_UP,
    KB_FN_ACTION_BL_SPEED_DOWN,
    KB_FN_ACTION_BL_SET_SCRIPT,
    KB_FN_ACTION_BL_SET_BRIGHTNESS,
    KB_FN_ACTION_BL_SET_SPEED,
} kb_fn_action_t;

typedef struct {
    uint8_t key;
    kb_fn_action_t action;
    int16_t param;
} kb_fn_shortcut_t;

typedef struct {

    bool enabled;
    kb_mouseemu_direction_t direction_mode;

    uint8_t move_keys_count;
    uint16_t move_keys[KB_MOUSEEMU_MOVE_KEYS_MAX];

    uint8_t scroll_keys_count;
    uint16_t scroll_keys[KB_MOUSEEMU_SCROLL_KEYS_MAX];

    uint8_t button_keys_count;
    uint16_t button_keys[KB_MOUSEEMU_BUTTON_KEYS_MAX];

    double move_x_k;
    double move_y_k;
    double scroll_k;

} kb_mouseemu_settings_t;

typedef struct {

    kb_mode_t mode;

    uint16_t thresholds[TOTAL_KEY_COUNT];
    uint16_t minimums[TOTAL_KEY_COUNT];
    uint16_t maximums[TOTAL_KEY_COUNT];
    uint16_t deadzones[TOTAL_KEY_COUNT];

    uint8_t mappings_layer1[TOTAL_KEY_COUNT];
    uint8_t mappings_layer2[TOTAL_KEY_COUNT];
    uint8_t mappings_layer3[TOTAL_KEY_COUNT];

    kb_mouseemu_settings_t mouseemu;

#if CONFIG_YKB_BATTSENSE
    ykb_battsense_settings_t battsense;
#endif // CONFIG_YKB_BATTSENSE

    enum kb_handler_transport_priority kbh_prio;

    uint8_t fn_shortcuts_count;
    kb_fn_shortcut_t fn_shortcuts[CONFIG_KB_SETTINGS_FN_SHORTCUTS_MAX];

#if CONFIG_YKB_BACKLIGHT
    ykb_backlight_settings_t backlight;
#endif // CONFIG_YKB_BACKLIGHT

} kb_settings_t;

struct kb_settings_cb {
    void (*on_update)(const kb_settings_t *settings);
};

#define ON_SETTINGS_UPDATE_DEFINE(name, cb)                                    \
    STRUCT_SECTION_ITERABLE(kb_settings_cb, name) = {.on_update = cb}

// Copies settings to the provided kb_settings_t pointer.
//
// This function will block until settings are available to copy.
// Returns 0 on success, negative value otherwise.
int kb_settings_get(kb_settings_t *settings);

// Applies new settings and saves them
//
// This funciton will block until settings are availble to save.
// Returns 0 on success, negative value otherwise.
int kb_settings_apply(const kb_settings_t *settings);

#endif // KB_SETTINGS_H_
