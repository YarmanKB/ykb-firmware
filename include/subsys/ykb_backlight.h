#ifndef YKB_BACKLIGHT_H
#define YKB_BACKLIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_THREAD_SLEEP_MS 5

#define YKB_BACKLIGHT_MAX_ABS_BRIGHTNESS_PERCENT                               \
    Z_USER_PROP_OR(ykb_backlight_max_abs_brightness, 20)

typedef struct {
    size_t key_count;
    const uint16_t *led_map;
    const uint16_t *x_coordinates;
    const uint16_t *y_coordinates;
} ykb_backlight_layout_t;

typedef struct {
    bool on;
    uint16_t active_script_index;
    float speed;
    float brightness;
    uint32_t thread_sleep_ms;
} ykb_backlight_settings_t;

typedef struct {
    bool occupied;
    uint32_t size;
    char name[CONFIG_YKB_BL_SCRIPT_NAME_MAX_LEN + 1];
    uint8_t bytecode[CONFIG_YKB_BL_SCRIPT_SLOT_SIZE];
} ykb_backlight_script_slot_t;

typedef struct {
    ykb_backlight_script_slot_t slots[CONFIG_YKB_BL_SCRIPT_SLOT_COUNT];
} ykb_backlight_script_slots_t;

typedef void (*ykb_backlight_script_slot_update_cb_t)(uint16_t slot);

const ykb_backlight_layout_t *ykb_backlight_get_layout(void);

const ykb_backlight_settings_t *ykb_backlight_get_default_settings(void);

const ykb_backlight_script_slots_t *ykb_backlight_get_default_script_slots(void);
size_t ykb_backlight_get_script_slot_count(void);
int ykb_backlight_get_script_slot(uint16_t slot,
                                  ykb_backlight_script_slot_t *out);
int ykb_backlight_get_script_slot_crc32(uint16_t slot, uint32_t *out_crc32);
int ykb_backlight_set_script_slot(uint16_t slot,
                                  const ykb_backlight_script_slot_t *in);
int ykb_backlight_register_script_slot_update_cb(
    ykb_backlight_script_slot_update_cb_t cb);

#endif // YKB_BACKLIGHT_H
