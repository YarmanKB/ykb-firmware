#define DT_DRV_COMPAT kscan_enables

#include "kscan_common.h"

#include <subsys/ykb_metrics.h>

#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(kscan_enables, CONFIG_KSCAN_LOG_LEVEL);

struct kscan_enables_config {
    const struct adc_dt_spec channel;
    const struct gpio_dt_spec *enables;

    const uint16_t idx_offset;
    const uint16_t key_amount;

    const uint32_t settle_us;
};

struct kscan_enables_data {
    uint16_t *values;

    struct k_thread thread;
    k_thread_stack_t *stack;
};

static void kscan_enables_thread(void *kscan_dev, void *_, void *__) {
    const struct device *dev = kscan_dev;
    const struct kscan_enables_config *cfg = dev->config;
    struct kscan_enables_data *data = dev->data;

    LOG_INF("Starting KScan Enables thread");

    int err;

    // Turn off every enable just to make sure
    for (uint16_t i = 0; i < cfg->key_amount; ++i) {
        const struct gpio_dt_spec *en = &cfg->enables[i];
        err = gpio_pin_set_dt(en, 0);
        if (err) {
            LOG_ERR("Unable to set GPIO pin (%s:%d) (err %d)", en->port->name,
                    en->pin, err);
            return;
        }
    }

    while (true) {
        uint32_t scan_start = k_cycle_get_32();
        for (uint16_t i = 0; i < cfg->key_amount; ++i) {
            const struct gpio_dt_spec *en = &cfg->enables[i];
            err = gpio_pin_set_dt(en, 1);
            if (err) {
                LOG_ERR("Unable to set GPIO pin (%s:%d) (err %d)",
                        en->port->name, en->pin, err);
                return;
            }

            k_usleep(cfg->settle_us);

            uint16_t val = 0;
            err = read_io_channel(&cfg->channel, &val);
            if (err < 0) {
                LOG_ERR("Could not read ADC channel '%d' (%d)",
                        cfg->channel.channel_id, err);
                err = gpio_pin_set_dt(en, 0);
                if (err) {
                    LOG_WRN("Unable to set GPIO pin (%s:%d) (err %d)",
                            en->port->name, en->pin, err);
                }
                YKB_METRICS_KSCAN_READ_ERROR(dev, 0U);
                return;
            }
            YKB_METRICS_KSCAN_SAMPLE(dev, 0U, cfg->idx_offset + i, val,
                                     cfg->channel.resolution);
            STRUCT_SECTION_FOREACH(kscan_cb, callbacks) {
                if (callbacks->on_new_value) {
                    callbacks->on_new_value(cfg->idx_offset + i, val);
                }
            }
            data->values[i] = val;

            err = gpio_pin_set_dt(en, 0);
            if (err) {
                LOG_ERR("Unable to set GPIO pin (%s:%d) (err %d)",
                        en->port->name, en->pin, err);
                return;
            }
        }
        YKB_METRICS_KSCAN_SCAN_DONE(dev, 0U, cfg->key_amount,
                                    k_cycle_get_32() - scan_start);
    }
}

static int kscan_enables_get_key_amount(const struct device *dev) {
    const struct kscan_enables_config *cfg = dev->config;
    return cfg->key_amount;
}

static int kscan_enables_get_idx_offset(const struct device *dev) {
    const struct kscan_enables_config *cfg = dev->config;
    return cfg->idx_offset;
}

static int kscan_enables_get_values(const struct device *dev,
                                    uint16_t *values) {
    if (!values) {
        return -EINVAL;
    }
    const struct kscan_enables_config *cfg = dev->config;
    struct kscan_enables_data *data = dev->data;
    memcpy(values, data->values, cfg->key_amount * sizeof(uint16_t));

    return 0;
}

DEVICE_API(kscan, kscan_enables_api) = {
    .get_key_amount = kscan_enables_get_key_amount,
    .get_idx_offset = kscan_enables_get_idx_offset,
    .get_values = kscan_enables_get_values,
};

static int kscan_enables_init(const struct device *dev) {
    const struct kscan_enables_config *cfg = dev->config;
    struct kscan_enables_data *data = dev->data;

    for (uint16_t i = 0; i < cfg->key_amount; ++i) {
        if (!device_is_ready(cfg->enables[i].port)) {
            return -ENODEV;
        }
        int ret = gpio_pin_configure_dt(&cfg->enables[i], GPIO_OUTPUT_INACTIVE);
        if (ret) {
            return ret;
        }
    }

    if (!adc_is_ready_dt(&cfg->channel)) {
        LOG_ERR("ADC device '%s' is not ready", cfg->channel.dev->name);
        return -ENODEV;
    }
    int err = adc_channel_setup_dt(&cfg->channel);
    if (err < 0) {
        LOG_ERR("Could not setup ADC channel '%d' (%d)",
                cfg->channel.channel_id, err);
        return -ENODEV;
    }
    LOG_DBG("Successfully set up ADC channel %d", cfg->channel.channel_id);

    LOG_INF("KScan (Enables) ready: %u enables", cfg->key_amount);

    k_thread_create(&data->thread, data->stack,
                    CONFIG_KSCAN_ENABLES_THREAD_STACK_SIZE,
                    kscan_enables_thread, (void *)dev, NULL, NULL,
                    CONFIG_KSCAN_ENABLES_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&data->thread, "kscan_enables");

    return 0;
}

#define GPIO_SPEC_AND_COMMA(node_id, prop, idx)                                \
    GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

#define ADC_SPEC_AND_COMMA(node_id, prop, idx)                                 \
    ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

#define U16_PROP_ELEM_AND_COMMA(node_id, prop, idx)                            \
    DT_PROP_BY_IDX(node_id, prop, idx),

#define KSCAN_ENABLES_DEFINE(inst)                                             \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, io_channels) == 1,                     \
                 "Kscan Enables instance should have only one ADC channel.");  \
    static const struct gpio_dt_spec __kscan_enables_enables__##inst[] = {     \
        DT_INST_FOREACH_PROP_ELEM(inst, enable_gpios, GPIO_SPEC_AND_COMMA)};   \
    K_THREAD_STACK_DEFINE(__kscan_enables_thread_stack__##inst,                \
                          CONFIG_KSCAN_ENABLES_THREAD_STACK_SIZE);             \
    static const struct adc_dt_spec __kscan_enables_adc_channel__##inst[] = {  \
        DT_INST_FOREACH_PROP_ELEM(inst, io_channels, ADC_SPEC_AND_COMMA)};     \
                                                                               \
    static uint16_t __kscan_enables_values__##inst[DT_INST_PROP_LEN(           \
        inst, enable_gpios)] = {0};                                            \
                                                                               \
    static const struct kscan_enables_config __kscan_enables_config__##inst =  \
        {                                                                      \
            .channel = __kscan_enables_adc_channel__##inst[0],                 \
            .enables = __kscan_enables_enables__##inst,                        \
                                                                               \
            .key_amount = DT_INST_PROP_LEN(inst, enable_gpios),                \
                                                                               \
            .idx_offset = DT_INST_PROP(inst, idx_offset),                      \
                                                                               \
            .settle_us = DT_INST_PROP(inst, settle_us),                        \
    };                                                                         \
    static struct kscan_enables_data __kscan_enables_data__##inst = {          \
        .stack = __kscan_enables_thread_stack__##inst,                         \
        .values = __kscan_enables_values__##inst,                              \
    };                                                                         \
                                                                               \
    DEVICE_DT_INST_DEFINE(                                                     \
        inst, kscan_enables_init, NULL, &__kscan_enables_data__##inst,         \
        &__kscan_enables_config__##inst, POST_KERNEL,                          \
        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &kscan_enables_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_ENABLES_DEFINE)
