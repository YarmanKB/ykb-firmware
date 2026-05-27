#define DT_DRV_COMPAT ti_bq25185

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bq25185, CONFIG_CHARGER_LOG_LEVEL);

#define GPIO_SPEC_PRESENT(_spec) ((_spec).port != NULL)

enum bq25185_fault_detail {
    BQ25185_FAULT_NONE = 0,
    BQ25185_FAULT_RECOVERABLE_GROUP,
    BQ25185_FAULT_LATCHOFF_GROUP,
};

struct bq25185_config {
    struct gpio_dt_spec stat1_gpios;
    struct gpio_dt_spec stat2_gpios;
    struct gpio_dt_spec vin_present_gpios;
    struct gpio_dt_spec ce_gpios;
};

struct bq25185_data;

struct bq25185_pin_data {
    const struct gpio_dt_spec *spec;
    struct gpio_callback cb;
    struct bq25185_data *parent;
    bool state;
};

struct bq25185_data {
    const struct device *dev;
    struct k_mutex lock;
    struct k_work work;

    struct bq25185_pin_data stat1;
    struct bq25185_pin_data stat2;
    struct bq25185_pin_data vin_present;

    enum charger_status status;
    enum charger_online online;
    enum charger_health health;
    enum charger_charge_type charge_type;

    enum bq25185_fault_detail fault_detail;

    bool charge_enabled;
};

static int bq25185_read_logical_pin(const struct gpio_dt_spec *spec) {
    if (!GPIO_SPEC_PRESENT(*spec)) {
        return -ENODEV;
    }

    return gpio_pin_get_dt(spec);
}

static int bq25185_refresh_cached_inputs(const struct device *dev) {
    const struct bq25185_config *cfg = dev->config;
    struct bq25185_data *data = dev->data;
    int ret;

    ret = bq25185_read_logical_pin(&cfg->stat1_gpios);
    if (ret < 0) {
        return ret;
    }
    data->stat1.state = (ret != 0);

    ret = bq25185_read_logical_pin(&cfg->stat2_gpios);
    if (ret < 0) {
        return ret;
    }
    data->stat2.state = (ret != 0);

    if (GPIO_SPEC_PRESENT(cfg->vin_present_gpios)) {
        ret = bq25185_read_logical_pin(&cfg->vin_present_gpios);
        if (ret < 0) {
            return ret;
        }
        data->vin_present.state = (ret != 0);
    }

    return 0;
}

// Datasheet pin table:
//   HIGH HIGH -> charge complete, sleep mode, or charge disabled
//   HIGH LOW  -> charging
//   LOW  HIGH -> recoverable fault
//   LOW  LOW  -> non-recoverable / latch-off fault
static void bq25185_decode_state(const struct device *dev) {
    const struct bq25185_config *cfg = dev->config;
    struct bq25185_data *data = dev->data;

    const bool stat1 = data->stat1.state;
    const bool stat2 = data->stat2.state;

    bool vin_present = true;

    data->online = CHARGER_ONLINE_OFFLINE;
    data->status = CHARGER_STATUS_NOT_CHARGING;
    data->health = CHARGER_HEALTH_GOOD;
    data->charge_type = CHARGER_CHARGE_TYPE_NONE;
    data->fault_detail = BQ25185_FAULT_NONE;

    if (GPIO_SPEC_PRESENT(cfg->vin_present_gpios)) {
        vin_present = data->vin_present.state;
    }

    if (!vin_present) {
        // Without input present, HIGH/HIGH is ambiguous in the datasheet, so
        // reporting FULL here would be wrong
        data->online = CHARGER_ONLINE_OFFLINE;
        data->status = CHARGER_STATUS_NOT_CHARGING;
        data->health = CHARGER_HEALTH_GOOD;
        data->charge_type = CHARGER_CHARGE_TYPE_NONE;
        return;
    }

    data->online = CHARGER_ONLINE_FIXED;

    if (!stat1 && !stat2) {
        // HIGH HIGH: full, sleep, or disabled
        if (GPIO_SPEC_PRESENT(cfg->ce_gpios) && !data->charge_enabled) {
            data->status = CHARGER_STATUS_NOT_CHARGING;
            data->charge_type = CHARGER_CHARGE_TYPE_NONE;
            data->health = CHARGER_HEALTH_GOOD;
        } else {
            data->status = CHARGER_STATUS_FULL;
            data->charge_type = CHARGER_CHARGE_TYPE_NONE;
            data->health = CHARGER_HEALTH_GOOD;
        }
    } else if (!stat1 && stat2) {
        // HIGH LOW: normal charging
        data->status = CHARGER_STATUS_CHARGING;
        data->charge_type = CHARGER_CHARGE_TYPE_STANDARD;
        data->health = CHARGER_HEALTH_GOOD;
    } else if (stat1 && !stat2) {
        // LOW HIGH: recoverable fault
        data->status = CHARGER_STATUS_NOT_CHARGING;
        data->charge_type = CHARGER_CHARGE_TYPE_NONE;
        data->health = CHARGER_HEALTH_UNSPEC_FAILURE;
        data->fault_detail = BQ25185_FAULT_RECOVERABLE_GROUP;
    } else {
        // LOW LOW: non-recoverable / latch-off fault
        data->status = CHARGER_STATUS_NOT_CHARGING;
        data->charge_type = CHARGER_CHARGE_TYPE_NONE;
        data->health = CHARGER_HEALTH_UNSPEC_FAILURE;
        data->fault_detail = BQ25185_FAULT_LATCHOFF_GROUP;
    }
}

static void bq25185_work_handler(struct k_work *work) {
    struct bq25185_data *data = CONTAINER_OF(work, struct bq25185_data, work);
    const struct device *dev = data->dev;
    int ret;

    k_mutex_lock(&data->lock, K_FOREVER);

    ret = bq25185_refresh_cached_inputs(dev);
    if (ret == 0) {
        bq25185_decode_state(dev);
        LOG_DBG("stat1=%d stat2=%d vin=%d online=%d status=%d health=%d "
                "charge_type=%d ce=%d fault_detail=%d",
                data->stat1.state, data->stat2.state, data->vin_present.state,
                data->online, data->status, data->health, data->charge_type,
                data->charge_enabled, data->fault_detail);
    } else {
        LOG_ERR("failed to refresh GPIO inputs: %d", ret);
    }

    k_mutex_unlock(&data->lock);
}

static void bq25185_gpio_cb(const struct device *port, struct gpio_callback *cb,
                            gpio_port_pins_t pins) {
    struct bq25185_pin_data *pin =
        CONTAINER_OF(cb, struct bq25185_pin_data, cb);
    struct bq25185_data *data = pin->parent;

    k_work_submit(&data->work);
}

static int bq25185_prepare_input_gpio(const struct gpio_dt_spec *spec,
                                      struct bq25185_pin_data *pin,
                                      struct bq25185_data *data) {
    int ret;

    if (!GPIO_SPEC_PRESENT(*spec)) {
        return 0;
    }

    if (!gpio_is_ready_dt(spec)) {
        LOG_ERR("GPIO device %s not ready for pin %u", spec->port->name,
                spec->pin);
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(spec, GPIO_INPUT);
    if (ret) {
        LOG_ERR("failed to configure input pin %u on %s: %d", spec->pin,
                spec->port->name, ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_BOTH);
    if (ret) {
        LOG_ERR("failed to configure interrupt pin %u on %s: %d", spec->pin,
                spec->port->name, ret);
        return ret;
    }

    pin->spec = spec;
    pin->parent = data;
    pin->state = false;

    gpio_init_callback(&pin->cb, bq25185_gpio_cb, BIT(spec->pin));

    ret = gpio_add_callback_dt(spec, &pin->cb);
    if (ret) {
        LOG_ERR("failed to add callback pin %u on %s: %d", spec->pin,
                spec->port->name, ret);
        return ret;
    }

    return 0;
}

static int bq25185_prepare_ce_gpio(const struct gpio_dt_spec *spec,
                                   bool *charge_enabled) {
    int ret;

    if (!GPIO_SPEC_PRESENT(*spec)) {
        return 0;
    }

    if (!gpio_is_ready_dt(spec)) {
        LOG_ERR("CE GPIO device %s not ready for pin %u", spec->port->name,
                spec->pin);
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(spec, GPIO_OUTPUT_ACTIVE);
    if (ret) {
        LOG_ERR("failed to configure CE pin %u on %s: %d", spec->pin,
                spec->port->name, ret);
        return ret;
    }

    *charge_enabled = true;
    return 0;
}

static int bq25185_charge_enable(const struct device *dev, const bool enable) {
    const struct bq25185_config *cfg = dev->config;
    struct bq25185_data *data = dev->data;
    int ret;

    if (!GPIO_SPEC_PRESENT(cfg->ce_gpios)) {
        return -ENOTSUP;
    }

    k_mutex_lock(&data->lock, K_FOREVER);

    ret = gpio_pin_set_dt(&cfg->ce_gpios, enable ? 1 : 0);
    if (ret == 0) {
        data->charge_enabled = enable;
        bq25185_decode_state(dev);
    }

    k_mutex_unlock(&data->lock);

    return ret;
}

static int bq25185_get_property(const struct device *dev,
                                const charger_prop_t prop,
                                union charger_propval *val) {
    struct bq25185_data *data = dev->data;
    int ret = 0;

    k_mutex_lock(&data->lock, K_FOREVER);

    switch (prop) {
    case CHARGER_PROP_STATUS:
        val->status = data->status;
        break;

    case CHARGER_PROP_ONLINE:
        val->online = data->online;
        break;

    case CHARGER_PROP_HEALTH:
        val->health = data->health;
        break;

    case CHARGER_PROP_CHARGE_TYPE:
        val->charge_type = data->charge_type;
        break;

    default:
        ret = -ENOTSUP;
        break;
    }

    k_mutex_unlock(&data->lock);

    return ret;
}

static int bq25185_set_property(const struct device *dev,
                                const charger_prop_t prop,
                                const union charger_propval *val) {
    return -ENOTSUP;
}

static DEVICE_API(charger, bq25185_api) = {
    .get_property = bq25185_get_property,
    .set_property = bq25185_set_property,
    .charge_enable = bq25185_charge_enable,
};

static int bq25185_init(const struct device *dev) {
    const struct bq25185_config *cfg = dev->config;
    struct bq25185_data *data = dev->data;
    int ret;

    data->dev = dev;
    data->status = CHARGER_STATUS_UNKNOWN;
    data->online = CHARGER_ONLINE_OFFLINE;
    data->health = CHARGER_HEALTH_UNKNOWN;
    data->charge_type = CHARGER_CHARGE_TYPE_UNKNOWN;
    data->fault_detail = BQ25185_FAULT_NONE;
    data->charge_enabled = !GPIO_SPEC_PRESENT(cfg->ce_gpios);

    k_mutex_init(&data->lock);
    k_work_init(&data->work, bq25185_work_handler);

    ret = bq25185_prepare_input_gpio(&cfg->stat1_gpios, &data->stat1, data);
    if (ret) {
        return ret;
    }

    ret = bq25185_prepare_input_gpio(&cfg->stat2_gpios, &data->stat2, data);
    if (ret) {
        return ret;
    }

    ret = bq25185_prepare_input_gpio(&cfg->vin_present_gpios,
                                     &data->vin_present, data);
    if (ret) {
        return ret;
    }

    ret = bq25185_prepare_ce_gpio(&cfg->ce_gpios, &data->charge_enabled);
    if (ret) {
        return ret;
    }

    ret = bq25185_refresh_cached_inputs(dev);
    if (ret) {
        return ret;
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    bq25185_decode_state(dev);
    k_mutex_unlock(&data->lock);

    LOG_INF("BQ25185 initialized");
    return 0;
}

#define BQ25185_INIT(inst)                                                     \
    static const struct bq25185_config bq25185_config_##inst = {               \
        .stat1_gpios = GPIO_DT_SPEC_INST_GET(inst, stat1_gpios),               \
        .stat2_gpios = GPIO_DT_SPEC_INST_GET(inst, stat2_gpios),               \
        .vin_present_gpios =                                                   \
            GPIO_DT_SPEC_INST_GET_OR(inst, vin_present_gpios, {0}),            \
        .ce_gpios = GPIO_DT_SPEC_INST_GET_OR(inst, ce_gpios, {0}),             \
    };                                                                         \
                                                                               \
    static struct bq25185_data bq25185_data_##inst;                            \
                                                                               \
    DEVICE_DT_INST_DEFINE(inst, bq25185_init, NULL, &bq25185_data_##inst,      \
                          &bq25185_config_##inst, POST_KERNEL,                 \
                          CONFIG_CHARGER_INIT_PRIORITY, &bq25185_api);

DT_INST_FOREACH_STATUS_OKAY(BQ25185_INIT)
