#ifndef YKB_BATTSENSE_H
#define YKB_BATTSENSE_H

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    enum charger_status charge_status;
    uint8_t percentage;
} ykb_battsense_state_t;

typedef struct {
    uint8_t low_threshold;
    uint16_t thread_sleep_ms;
} ykb_battsense_settings_t;

typedef void (*ykb_battsense_cb_t)(ykb_battsense_state_t state);

struct ykb_battsense_cb {

    // Will run every time state of the battery changes
    ykb_battsense_cb_t on_state_changed;

    // Will run after crossing CONFIG_YKB_BATTSENSE_LOW_THRESHOLD while
    // discharging.
    ykb_battsense_cb_t on_low_percentage;

    // Will run after crossing CONFIG_YKB_BATTSENSE_CRIT_THRESHOLD while
    // discharging.
    // After all these callbacks are called the system will shutdown
    // if CONFIG_YKB_BATTSENSE_CRIT_SHUTDOWN is set.
    ykb_battsense_cb_t on_critical_percentage;
};

#if CONFIG_YKB_BATTSENSE
#define YKB_BATTSENSE_DEFINE(name)                                             \
    STRUCT_SECTION_ITERABLE(ykb_battsense_cb, __ykb_battsense_cb__##name)
#else
#define YKB_BATTSENSE_DEFINE(name)
#endif // CONFIG_YKB_BATTSENSE

int ykb_battsense_get_state(ykb_battsense_state_t *out_state);

const ykb_battsense_settings_t *ykb_battsense_get_default_settings(void);

#endif // YKB_BATTSENSE_H
