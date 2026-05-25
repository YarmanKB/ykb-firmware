#include <subsys/ykb_battsense.h>

#include <subsys/kb_settings.h>
#include <subsys/zephyr_user_helpers.h>

#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/emul_fuel_gauge.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ykb_battsense, CONFIG_YKB_BATTSENSE_LOG_LEVEL);

static const struct device *charger = Z_USER_DEV(ykb_battsense_charger);
static const struct device *fuel_gauge = Z_USER_DEV(ykb_battsense_fuel_gauge);
static const struct gpio_dt_spec pw_cutoff_gpio =
    GPIO_DT_SPEC_GET_OR(Z_USER_PATH, ykb_battsense_pw_cutoff_gpios, {0});

#define PW_CUTOFF_PRESENT IS_ENABLED(CONFIG_YKB_BATTSENSE_PW_CUTOFF_PRESENT)

static const ykb_battsense_settings_t battsense_default_settings = {
    .thread_sleep_ms = CONFIG_YKB_BATTSENSE_THREAD_DEFAULT_SLEEP_TIME,
    .low_threshold = CONFIG_YKB_BATTSENSE_LOW_THRESHOLD,
};

static ykb_battsense_settings_t battsense_settings = {
    .thread_sleep_ms = CONFIG_YKB_BATTSENSE_THREAD_DEFAULT_SLEEP_TIME,
    .low_threshold = CONFIG_YKB_BATTSENSE_LOW_THRESHOLD,
};

static ykb_battsense_state_t state;

static K_MUTEX_DEFINE(battsense_mut);

static struct k_thread battsense_thread;
static K_THREAD_STACK_DEFINE(battsense_thread_stack,
                             CONFIG_YKB_BATTSENSE_THREAD_STACK_SIZE);

static void battsense_thread_handler(void *a, void *b, void *c) {
    while (true) {
        k_mutex_lock(&battsense_mut, K_FOREVER);

        union charger_propval charger_state;
        union fuel_gauge_prop_val rsoc;
        int err;

#if CONFIG_YKB_BATTSENSE_SHUTOFF_ON_CHARGER_FAILURE
        err = charger_get_prop(charger, CHARGER_PROP_HEALTH, &charger_state);
        if (err && err != -ENOTSUP) {
            LOG_ERR("Unable to get battery health: err %d", err);
            // Not sure if need to do anything about it
        }
        if (!err && charger_state.health == CHARGER_HEALTH_UNSPEC_FAILURE) {
            LOG_ERR("Charger health is CHARGER_HEALTH_UNSPEC_FAILURE, shutting "
                    "down.");
            // TODO: maybe we can do some quick deinit before doing that
            k_mutex_unlock(&battsense_mut);
            gpio_pin_set_dt(&pw_cutoff_gpio, 1);
            return;
        }
#endif // CONFIG_YKB_BATTSENSE_SHUTOFF_ON_CHARGER_FAILURE

        err = charger_get_prop(charger, CHARGER_PROP_STATUS, &charger_state);
        if (err) {
            LOG_ERR("Unable to get charger property status: %d", err);
            k_mutex_unlock(&battsense_mut);
            continue;
        }

        err = fuel_gauge_get_prop(fuel_gauge,
                                  FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &rsoc);
        if (err) {
            LOG_ERR("Unable to get fuel-gauge property RSOC: %d", err);
            k_mutex_unlock(&battsense_mut);
            continue;
        }

        ykb_battsense_state_t old_state = state;
        state.charge_status = charger_state.status;
        state.percentage = MIN(rsoc.relative_state_of_charge, 100);
        LOG_DBG("state: charge status %d, percentage %d", state.charge_status,
                state.percentage);

        if (old_state.percentage > state.percentage &&
            state.percentage <= CONFIG_YKB_BATTSENSE_CRIT_THRESHOLD) {
            STRUCT_SECTION_FOREACH(ykb_battsense_cb, cb) {
                if (cb->on_critical_percentage) {
                    cb->on_critical_percentage(state);
                }
            }
            if (PW_CUTOFF_PRESENT) {
                // TODO: maybe we can do some quick deinit before doing that
                k_mutex_unlock(&battsense_mut);
                gpio_pin_set_dt(&pw_cutoff_gpio, 1);
                return;
            }
        } else if (old_state.percentage > state.percentage &&
                   state.percentage <= battsense_settings.low_threshold) {
            STRUCT_SECTION_FOREACH(ykb_battsense_cb, cb) {
                if (cb->on_low_percentage) {
                    cb->on_low_percentage(state);
                }
            }
        } else if (old_state.percentage != state.percentage ||
                   old_state.charge_status != state.charge_status) {
            LOG_INF("State changed: Pecentage: %d, Status: %d",
                    state.percentage, state.charge_status);
            STRUCT_SECTION_FOREACH(ykb_battsense_cb, cb) {
                if (cb->on_state_changed) {
                    cb->on_state_changed(state);
                }
            }
        }

        uint32_t sleep_time = battsense_settings.thread_sleep_ms;
        k_mutex_unlock(&battsense_mut);
        k_sleep(K_MSEC(sleep_time));
    }
}

static int ykb_battsense_init(void) {
    int err;

    if (PW_CUTOFF_PRESENT) {
        if (!gpio_is_ready_dt(&pw_cutoff_gpio)) {
            LOG_ERR("pw_cutoff_gpio is not ready.");
            return -1;
        }
        err = gpio_pin_configure_dt(&pw_cutoff_gpio, GPIO_OUTPUT_INACTIVE);
        if (err) {
            LOG_ERR("pw_cutoff_gpio configure: err %d", err);
            return err;
        }
    }

    if (!device_is_ready(charger)) {
        LOG_ERR("Charger device %s is not ready.", charger->name);
        return -1;
    }
    if (!device_is_ready(fuel_gauge)) {
        LOG_ERR("Fuel-gauge device %s is not ready.", fuel_gauge->name);
        return -1;
    }

    // Enable charging if needed
    err = charger_charge_enable(charger, true);
    if (err) {
        if (err == -ENOTSUP) {
            LOG_DBG(
                "Charging enabling for charger %s is not supported, skipping.",
                charger->name);
        } else {
            LOG_ERR("Unable to enable charging for charger %s, err %d",
                    charger->name, err);
            return -1;
        }
    }

    k_thread_create(&battsense_thread, battsense_thread_stack,
                    K_THREAD_STACK_SIZEOF(battsense_thread_stack),
                    battsense_thread_handler, NULL, NULL, NULL,
                    CONFIG_YKB_BATTSENSE_THREAD_PRIORITY, 0, K_NO_WAIT);

    LOG_INF("BattSense init ok.");

    return 0;
}

SYS_INIT(ykb_battsense_init, POST_KERNEL, CONFIG_YKB_BATTSENSE_INIT_PRIORITY);

static void on_settings_update(const kb_settings_t *settings) {
    k_mutex_lock(&battsense_mut, K_FOREVER);
    memcpy(&battsense_settings, &settings->battsense,
           sizeof(ykb_battsense_settings_t));
    k_mutex_unlock(&battsense_mut);
}

ON_SETTINGS_UPDATE_DEFINE(ykb_battsense, on_settings_update);

int ykb_battsense_get_state(ykb_battsense_state_t *out_state) {
    if (!out_state) {
        return -EINVAL;
    }

    k_mutex_lock(&battsense_mut, K_FOREVER);
    *out_state = state;
    k_mutex_unlock(&battsense_mut);

    return 0;
}

const ykb_battsense_settings_t *ykb_battsense_get_default_settings(void) {
    return &battsense_default_settings;
}
