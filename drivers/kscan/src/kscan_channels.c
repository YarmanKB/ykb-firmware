#define DT_DRV_COMPAT kscan_channels

#include "kscan_common.h"

#include <subsys/ykb_metrics.h>

LOG_MODULE_REGISTER(kscan_channels, CONFIG_KSCAN_LOG_LEVEL);

struct kscan_channels_config {
    const struct adc_dt_spec *channels;
    const uint16_t channels_count;

    const uint32_t settle_us;
    const uint16_t idx_offset;
};

struct kscan_channels_data {
    struct k_thread *threads;
    k_thread_stack_t **stacks;
    uint16_t *chan_idxs;

    uint16_t *values;
};

// Thread which will run for each adc_chan
static void kscan_channels_thread(void *device, void *chan_idx, void *__) {
    const struct device *dev = device;
    const uint16_t idx = *(uint16_t *)chan_idx;
    LOG_INF("Starting KScan Channels thread for channel index %d", idx);

    const struct kscan_channels_config *cfg = dev->config;
    struct kscan_channels_data *data = dev->data;
    const struct adc_dt_spec chan = cfg->channels[idx];

    while (true) {
        uint32_t scan_start = k_cycle_get_32();
        uint16_t val = 0;
        int err = read_io_channel(&chan, &val);
        if (err < 0) {
            LOG_ERR("[%d] Could not read ADC channel '%d' (%d)", idx,
                    chan.channel_id, err);
            YKB_METRICS_KSCAN_READ_ERROR(dev, idx);
            return;
        }
        YKB_METRICS_KSCAN_SAMPLE(dev, idx, cfg->idx_offset + idx, val,
                                 chan.resolution);
        STRUCT_SECTION_FOREACH(kscan_cb, callbacks) {
            if (callbacks->on_new_value) {
                callbacks->on_new_value(cfg->idx_offset + idx, val);
            }
        }

        data->values[idx] = val;
        YKB_METRICS_KSCAN_SCAN_DONE(dev, idx, 1U,
                                    k_cycle_get_32() - scan_start);
        k_usleep(cfg->settle_us);
    }
    return;
}

static int kscan_channels_get_key_amount(const struct device *dev) {
    const struct kscan_channels_config *cfg = dev->config;
    return cfg->channels_count;
}

static int kscan_channels_get_idx_offset(const struct device *dev) {
    const struct kscan_channels_config *cfg = dev->config;
    return cfg->idx_offset;
}

static int kscan_channels_get_values(const struct device *dev,
                                     uint16_t *values) {
    if (!values) {
        return -EINVAL;
    }

    const struct kscan_channels_config *cfg = dev->config;
    struct kscan_channels_data *data = dev->data;

    memcpy(values, data->values, cfg->channels_count * sizeof(uint16_t));

    return 0;
}

DEVICE_API(kscan, kscan_channels_api) = {
    .get_key_amount = kscan_channels_get_key_amount,
    .get_idx_offset = kscan_channels_get_idx_offset,
    .get_values = kscan_channels_get_values,
};

static int kscan_channels_init(const struct device *dev) {
    const struct kscan_channels_config *cfg = dev->config;
    struct kscan_channels_data *data = dev->data;

    for (uint16_t i = 0; i < cfg->channels_count; ++i) {
        const struct adc_dt_spec *adc_spec = &cfg->channels[i];
        if (!adc_is_ready_dt(adc_spec)) {
            LOG_ERR("ADC device '%s' is not ready", adc_spec->dev->name);
            return -ENODEV;
        }
        int err = adc_channel_setup_dt(adc_spec);
        if (err < 0) {
            LOG_ERR("Could not setup ADC channel '%d' (%d)",
                    adc_spec->channel_id, err);
            return -ENODEV;
        }
        LOG_DBG("Successfully set up ADC channel %d", adc_spec->channel_id);
    }

    LOG_INF("KScan (channels) ready: %u channels", cfg->channels_count);

    for (uint16_t i = 0; i < cfg->channels_count; ++i) {
        k_thread_create(&data->threads[i], data->stacks[i],
                        CONFIG_KSCAN_CHANNELS_THREAD_STACK_SIZE,
                        kscan_channels_thread, (void *)dev, &data->chan_idxs[i],
                        NULL, CONFIG_KSCAN_CHANNELS_THREAD_PRIORITY, 0,
                        K_NO_WAIT);

        k_thread_name_set(&data->threads[i], "kscan_channels");
    }

    return 0;
}

#define THREAD_STACK_NAME(node_id, idx)                                        \
    __kscan_channels_stack__##node_id##_##idx

#define THREAD_STACK_REFERENCE_IDX_AND_COMMA(node_id, prop, idx)               \
    __kscan_channels_stack__##node_id##_##idx,

#define U16_PROP_ELEM_AND_COMMA(node_id, prop, idx)                            \
    DT_PROP_BY_IDX(node_id, prop, idx),

#define THREAD_STACK_DEFINE_IDX(node_id, prop, idx)                            \
    K_THREAD_STACK_DEFINE(THREAD_STACK_NAME(node_id, idx),                     \
                          CONFIG_KSCAN_CHANNELS_THREAD_STACK_SIZE);

#define CHANNEL_IDX_AND_COMMA(node_id, prop, idx) idx,

#define ADC_SPEC_AND_COMMA(node_id, prop, idx)                                 \
    ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

#define KSCAN_CHANNELS_DEFINE(inst)                                            \
    static const struct adc_dt_spec __kscan_channels_adc_channels__##inst[] =  \
        {DT_INST_FOREACH_PROP_ELEM(inst, io_channels, ADC_SPEC_AND_COMMA)};    \
    DT_INST_FOREACH_PROP_ELEM(inst, io_channels, THREAD_STACK_DEFINE_IDX);     \
    enum {                                                                     \
        __kscan_channels_cnt__##inst = DT_INST_PROP_LEN(inst, io_channels)     \
    };                                                                         \
    static uint16_t                                                            \
        __kscan_channels_values__##inst[__kscan_channels_cnt__##inst] = {0};   \
    static k_thread_stack_t                                                    \
        *__kscan_channels_stacks__##inst[__kscan_channels_cnt__##inst] = {     \
            DT_INST_FOREACH_PROP_ELEM(inst, io_channels,                       \
                                      THREAD_STACK_REFERENCE_IDX_AND_COMMA)};  \
    static struct k_thread                                                     \
        __kscan_channels_threads__##inst[__kscan_channels_cnt__##inst] = {};   \
    static uint16_t                                                            \
        __kscan_channels_chan_idxs__##inst[__kscan_channels_cnt__##inst] = {   \
            DT_INST_FOREACH_PROP_ELEM(inst, io_channels,                       \
                                      CHANNEL_IDX_AND_COMMA)};                 \
    static const struct kscan_channels_config                                  \
        __kscan_channels_config__##inst = {                                    \
            .channels = __kscan_channels_adc_channels__##inst,                 \
            .channels_count = DT_INST_PROP_LEN(inst, io_channels),             \
                                                                               \
            .idx_offset = DT_INST_PROP(inst, idx_offset),                      \
                                                                               \
            .settle_us = DT_INST_PROP(inst, settle_us),                        \
    };                                                                         \
                                                                               \
    static struct kscan_channels_data __kscan_channels_data__##inst = {        \
        .threads = __kscan_channels_threads__##inst,                           \
        .stacks = __kscan_channels_stacks__##inst,                             \
        .chan_idxs = __kscan_channels_chan_idxs__##inst,                       \
        .values = __kscan_channels_values__##inst,                             \
    };                                                                         \
                                                                               \
    DEVICE_DT_INST_DEFINE(                                                     \
        inst, kscan_channels_init, NULL, &__kscan_channels_data__##inst,       \
        &__kscan_channels_config__##inst, POST_KERNEL,                         \
        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &kscan_channels_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_CHANNELS_DEFINE)
