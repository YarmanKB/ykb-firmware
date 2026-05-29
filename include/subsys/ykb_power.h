#ifndef YKB_POWER_H
#define YKB_POWER_H

#include <zephyr/devicetree.h>
#include <zephyr/devicetree_generated.h>
#include <zephyr/sys/clock.h>

// TODO: This is currently just a manager for power buttons and power
// indicators. But eventually this should also become a subsystem for full
// power management (sleep modes, managing core clocks, etc)

#define YKB_PWR_IND_PRESENT IS_ENABLED(CONFIG_YKB_POWER_INDICATOR_PRESENT)
#define YKB_PWR_IND_BRIGHTNESS_SUPPORT                                         \
    IS_ENABLED(CONFIG_YKB_POWER_INDICATOR_BRIGHTNESS_SUPPORT)
#define YKB_PWR_BTN_PRESENT IS_ENABLED(CONFIG_YKB_POWER_BUTTON_PRESENT)
#define YKB_PWR_SHDN_PRESENT IS_ENABLED(CONFIG_YKB_POWER_SHUTDOWN_GPIOS_PRESENT)
#define YKB_PWR_BTN_SHDN_SUPPORT (YKB_PWR_BTN_PRESENT && YKB_PWR_SHDN_PRESENT)

typedef enum {

    YKB_POWER_INDICATOR_MODE_OFF = 0U,
    YKB_POWER_INDICATOR_MODE_STATIC,
    YKB_POWER_INDICATOR_MODE_BLINKING,

#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
    YKB_POWER_INDICATOR_MODE_BREATHE,
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT

} ykb_power_indicator_mode_t;

typedef struct {

#if YKB_PWR_IND_PRESENT

    ykb_power_indicator_mode_t pwr_indicator_mode;

    // For blinking this should be time (ms) between on & off
    // For breathe this should be time (ms) of one full breathe cycle
    uint32_t speed;

#if YKB_PWR_IND_BRIGHTNESS_SUPPORT

    // Valid for basically every mode if LED brightness can be controlled
    // Value [0-255] I think
    uint8_t brightness;

#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT

#endif // YKB_PWR_IND_PRESENT

#if YKB_PWR_BTN_SHDN_SUPPORT

    // If both power button and shutdown gpio is present
    // we can configure the amount of time power button
    // needs to be pressed before the shutdown gpio is triggered.
    // (Milliseconds).
    uint32_t shutdown_time;

#endif // YKB_PWR_BTN_SHDN_SUPPORT

} ykb_power_settings_t;

// Schedule a system shutdown if 'ykb-power-shutdown-gpios' present.
// Returns 0 on success, negative error on failure
// (-ENODEV if shutdown gpio not present).
int ykb_power_schedule_shutdown(k_timeout_t timeout);
const ykb_power_settings_t *ykb_power_get_default_settings(void);

#endif // YKB_POWER_H
