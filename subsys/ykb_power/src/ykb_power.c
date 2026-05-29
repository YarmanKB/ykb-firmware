#include <subsys/ykb_power.h>

#include <subsys/kb_settings.h>
#include <subsys/zephyr_user_helpers.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ykb_power, CONFIG_YKB_POWER_LOG_LEVEL);

#if YKB_PWR_IND_PRESENT
static const struct led_dt_spec pwr_ind =
    LED_DT_SPEC_GET(Z_USER_PROP(ykb_power_indicator));
#endif // YKB_PWR_IND_PRESENT

#if YKB_PWR_BTN_PRESENT
// TODO: Ok for now, but we should instead use the button API
static const struct gpio_dt_spec pwr_btn_gpio =
    GPIO_DT_SPEC_GET(Z_USER_PROP(ykb_power_button), gpios);
#endif // YKB_PWR_BTN_PRESENT

#if YKB_PWR_SHDN_PRESENT
static const struct gpio_dt_spec shdn_gpio =
    GPIO_DT_SPEC_GET(Z_USER_PATH, ykb_power_shutdown_gpios);
#endif // YKB_PWR_SHDN_PRESENT

static K_THREAD_STACK_DEFINE(ykb_power_thread_stack,
                             CONFIG_YKB_POWER_THREAD_STACK_SIZE);
static struct k_thread ykb_power_thread;
static K_MUTEX_DEFINE(ykb_power_mut);

#if YKB_PWR_SHDN_PRESENT
static void ykb_power_shutdown_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ykb_power_shutdown_work,
                               ykb_power_shutdown_work_handler);
#endif // YKB_PWR_SHDN_PRESENT

static const ykb_power_settings_t ykb_power_default_settings = {
#if YKB_PWR_IND_PRESENT
#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
    .pwr_indicator_mode = YKB_POWER_INDICATOR_MODE_BREATHE,
#else
    .pwr_indicator_mode = YKB_POWER_INDICATOR_MODE_STATIC,
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT
    .speed = 4000U,
#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
    .brightness = LED_BRIGHTNESS_MAX,
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT
#endif // YKB_PWR_IND_PRESENT
#if YKB_PWR_BTN_SHDN_SUPPORT
    .shutdown_time = 1000U,
#endif // YKB_PWR_BTN_SHDN_SUPPORT
};

static ykb_power_settings_t settings_snap;

#if YKB_PWR_SHDN_PRESENT
static void ykb_power_trigger_shutdown(void) {
    int err = gpio_pin_set_dt(&shdn_gpio, 1);

    if (err) {
        LOG_ERR("Shutdown gpio set: %d", err);
    }
}

static void ykb_power_shutdown_work_handler(struct k_work *work) {

    ykb_power_trigger_shutdown();
}
#endif // YKB_PWR_SHDN_PRESENT

#if YKB_PWR_IND_PRESENT

static void ykb_power_handle_power_indicator(void) {
    int err;
#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
    uint8_t brightness =
        MIN(settings_snap.brightness, (uint8_t)LED_BRIGHTNESS_MAX);
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT

    if (settings_snap.pwr_indicator_mode == YKB_POWER_INDICATOR_MODE_OFF) {
        err = led_off_dt(&pwr_ind);
        if (err) {
            LOG_ERR("led_off: %d", err);
        }
    }
    if (settings_snap.pwr_indicator_mode == YKB_POWER_INDICATOR_MODE_STATIC) {
        err = led_on_dt(&pwr_ind);
        if (err) {
            LOG_ERR("led_on: %d", err);
        }
    }
    if (settings_snap.pwr_indicator_mode == YKB_POWER_INDICATOR_MODE_BLINKING) {
        err = led_blink(pwr_ind.dev, pwr_ind.index, settings_snap.speed,
                        settings_snap.speed);
        if (err) {
            LOG_ERR("led_blink: %d", err);
        }
    }
#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
    if (settings_snap.pwr_indicator_mode != YKB_POWER_INDICATOR_MODE_BREATHE) {
        err = led_set_brightness_dt(&pwr_ind, brightness);
        if (err) {
            LOG_ERR("led_set_brightness: %d", err);
        }
    }
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT
}

#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
static void ykb_power_handle_power_indicator_breathe_mode(void) {
    uint32_t cycle_ms;
    uint32_t phase_ms;
    uint32_t rising_ms;
    uint32_t falling_ms;
    uint32_t peak;
    uint32_t level;
    uint8_t brightness;
    int err;

    if (settings_snap.pwr_indicator_mode != YKB_POWER_INDICATOR_MODE_BREATHE) {
        return;
    }

    cycle_ms = MAX(settings_snap.speed, 2U);
    phase_ms = k_uptime_get_32() % cycle_ms;
    rising_ms = cycle_ms / 2U;
    falling_ms = cycle_ms - rising_ms;
    peak = MIN(settings_snap.brightness, (uint8_t)LED_BRIGHTNESS_MAX);

    if (phase_ms < rising_ms) {
        level = (phase_ms * peak) / rising_ms;
    } else {
        uint32_t fall_phase = phase_ms - rising_ms;
        level = ((falling_ms - fall_phase) * peak) / falling_ms;
    }

    brightness = (uint8_t)MIN(level, (uint32_t)LED_BRIGHTNESS_MAX);
    err = led_set_brightness_dt(&pwr_ind, brightness);
    if (err) {
        LOG_ERR("led_set_brightness: %d", err);
    }
}
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT

#endif // YKB_PWR_IND_PRESENT

#if YKB_PWR_BTN_SHDN_SUPPORT
static void ykb_power_handle_power_button_shutdown(void) {
    static int64_t pressed_since_ms;
    static bool shutdown_requested;
    int value;
    int64_t now_ms;

    value = gpio_pin_get_dt(&pwr_btn_gpio);
    if (value < 0) {
        LOG_ERR("Power button GPIO read: %d", value);
        return;
    }

    if (value == 0) {
        pressed_since_ms = 0;
        shutdown_requested = false;
        return;
    }

    now_ms = k_uptime_get();
    if (pressed_since_ms == 0) {
        pressed_since_ms = now_ms;
        return;
    }

    if (shutdown_requested) {
        return;
    }

    if ((uint32_t)(now_ms - pressed_since_ms) >= settings_snap.shutdown_time) {
        shutdown_requested = true;
        ykb_power_trigger_shutdown();
    }
}
#endif // YKB_PWR_BTN_SHDN_SUPPORT

static void ykb_power_thread_handler(void *a, void *b, void *c) {
    while (true) {
        k_mutex_lock(&ykb_power_mut, K_FOREVER);

#if YKB_PWR_IND_BRIGHTNESS_SUPPORT
        ykb_power_handle_power_indicator_breathe_mode();
#endif // YKB_PWR_IND_BRIGHTNESS_SUPPORT

#if YKB_PWR_BTN_SHDN_SUPPORT
        ykb_power_handle_power_button_shutdown();
#endif // YKB_PWR_BTN_SHDN_SUPPORT

        k_mutex_unlock(&ykb_power_mut);
        k_sleep(K_MSEC(CONFIG_YKB_POWER_THREAD_SLEEP_TIME));
    }
}

static int ykb_power_init(void) {
    int err;

#if YKB_PWR_IND_PRESENT
    if (!led_is_ready_dt(&pwr_ind)) {
        LOG_ERR("Power indicator device is not ready.");
        return -1;
    }
#endif // YKB_PWR_IND_PRESENT

#if YKB_PWR_BTN_PRESENT
    if (!gpio_is_ready_dt(&pwr_btn_gpio)) {
        LOG_ERR("Power button GPIO is not ready.");
        return -1;
    }
    err = gpio_pin_configure_dt(&pwr_btn_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Power button GPIO configure: %d", err);
        return -1;
    }
#endif // YKB_PWR_BTN_PRESENT

#if YKB_PWR_SHDN_PRESENT
    if (!gpio_is_ready_dt(&shdn_gpio)) {
        LOG_ERR("Shutdown gpio is not ready.");
        return -1;
    }
    err = gpio_pin_configure_dt(&shdn_gpio, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Shutdown gpio configure: %d", err);
        return -1;
    }
#endif // YKB_PWR_SHDN_PRESENT

    memcpy(&settings_snap, &ykb_power_default_settings, sizeof(settings_snap));

    k_thread_create(&ykb_power_thread, ykb_power_thread_stack,
                    CONFIG_YKB_POWER_THREAD_STACK_SIZE,
                    ykb_power_thread_handler, NULL, NULL, NULL,
                    CONFIG_YKB_POWER_THREAD_PRIORITY, 0, K_NO_WAIT);
    return 0;
}

SYS_INIT(ykb_power_init, POST_KERNEL, CONFIG_YKB_POWER_INIT_PRIORITY);

static void on_settings_update(const kb_settings_t *settings) {
    k_mutex_lock(&ykb_power_mut, K_FOREVER);

    memcpy(&settings_snap, &settings->power, sizeof(settings_snap));

#if YKB_PWR_IND_PRESENT
    ykb_power_handle_power_indicator();
#endif // YKB_PWR_IND_PRESENT

    k_mutex_unlock(&ykb_power_mut);
}

ON_SETTINGS_UPDATE_DEFINE(ykb_power, on_settings_update);

const ykb_power_settings_t *ykb_power_get_default_settings(void) {
    return &ykb_power_default_settings;
}

int ykb_power_schedule_shutdown(k_timeout_t timeout) {
#if YKB_PWR_SHDN_PRESENT
    int ret = k_work_schedule(&ykb_power_shutdown_work, timeout);

    if (ret < 0) {
        return ret;
    }

    return 0;
#else
    return -ENODEV;
#endif // YKB_PWR_SHDN_PRESENT
}
