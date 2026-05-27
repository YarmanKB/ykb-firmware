#define DT_DRV_COMPAT kscan_muxes

#include "kscan_common.h"

#include <drivers/mux.h>
#include <subsys/ykb_metrics.h>

LOG_MODULE_REGISTER(kscan_muxes, CONFIG_KSCAN_LOG_LEVEL);

struct kscan_muxes_config {
    const struct adc_dt_spec *channels;
    const uint16_t channels_count;

    const uint16_t idx_offset;
    const uint16_t key_amount;

    const struct device **muxes;
    const uint16_t muxes_count;

    const uint32_t settle_us;
};

struct kscan_muxes_data {
    uint16_t *values;

    struct k_thread *threads;
    k_thread_stack_t **stacks;
    uint16_t *chan_idxs;
};

// Thread which will run for each mux/adc_chan pair
static void kscan_muxes_thread(void *kscan_dev, void *chan_index, void *_) {

    const uint16_t chan_idx = *((uint16_t *)chan_index);
    LOG_INF("Starting KScan MUXes thread for channel index %d", chan_idx);

    const struct device *dev = kscan_dev;
    const struct kscan_muxes_config *cfg = dev->config;
    struct kscan_muxes_data *data = dev->data;

    const struct adc_dt_spec chan = cfg->channels[chan_idx];

    // Get the offset of the current multiplexor by summing up all previous
    // muxes channels
    uint16_t chan_offset = 0;
    for (uint16_t i = 0; i < chan_idx; ++i) {
        const struct device *mux = cfg->muxes[i];
        int mux_channels = mux_get_channel_amount(mux);
        if (mux_channels < 0) {
            LOG_ERR("[%d] Unable to get mux '%s' channels (err %d)", chan_idx,
                    mux->name, mux_channels);
            return;
        }
        chan_offset += mux_channels;
    }

    // Get the amount of channels for current mux
    const struct device *mux = cfg->muxes[chan_idx];
    int mux_channels = mux_get_channel_amount(mux);
    if (mux_channels < 0) {
        LOG_ERR("[%d] Unable to get mux '%s' channels (err %d)", chan_idx,
                mux->name, mux_channels);
        return;
    }

    int err = 0;
    int lerr = 0;

    err = mux_select(mux, 0);
    if (err < 0) {
        LOG_ERR("[%d] Unable to select first channel on mux '%s' (err %d)",
                chan_idx, mux->name, err);
        return;
    }
    err = mux_enable(mux);
    if (err < 0) {
        LOG_ERR("[%d] Unable to enable mux '%s' (err %d)", chan_idx, mux->name,
                err);
        return;
    }

    while (true) {
        uint32_t scan_start = k_cycle_get_32();
        for (int i = 0; i < mux_channels; ++i) {
            uint16_t val = 0;
            err = read_io_channel(&chan, &val);
            if (err < 0) {
                LOG_ERR("[%d] Could not read ADC channel '%d' (%d)", chan_idx,
                        chan.channel_id, err);
                YKB_METRICS_KSCAN_READ_ERROR(dev, chan_idx);
                goto cleanup;
            }
            uint16_t kscan_offset = chan_offset + i;
            YKB_METRICS_KSCAN_SAMPLE(dev, chan_idx,
                                     cfg->idx_offset + kscan_offset, val,
                                     chan.resolution);
            STRUCT_SECTION_FOREACH(kscan_cb, callbacks) {
                if (callbacks->on_new_value) {
                    callbacks->on_new_value(cfg->idx_offset + kscan_offset,
                                            val);
                }
            }
            data->values[kscan_offset] = val;
            err = mux_select_next(mux);
            if (err < 0) {
                LOG_ERR("[%d] Unable to select next channel of the mux '%s'",
                        chan_idx, mux->name);
                goto cleanup;
            }
            k_usleep(cfg->settle_us);
        }
        YKB_METRICS_KSCAN_SCAN_DONE(dev, chan_idx, mux_channels,
                                    k_cycle_get_32() - scan_start);
    }

cleanup:
    lerr = mux_disable(mux);
    if (lerr < 0) {
        LOG_WRN("Unable to disable mux '%s' (err %d)", mux->name, err);
    }
    return;
}

static int kscan_muxes_get_key_amount(const struct device *dev) {
    const struct kscan_muxes_config *cfg = dev->config;
    return cfg->key_amount;
}

static int kscan_muxes_get_idx_offset(const struct device *dev) {
    const struct kscan_muxes_config *cfg = dev->config;
    return cfg->idx_offset;
}

static int kscan_muxes_get_values(const struct device *dev, uint16_t *values) {
    if (!values) {
        return -EINVAL;
    }
    const struct kscan_muxes_config *cfg = dev->config;
    struct kscan_muxes_data *data = dev->data;
    memcpy(values, data->values, sizeof(uint16_t) * cfg->key_amount);

    return 0;
}

DEVICE_API(kscan, kscan_muxes_api) = {
    .get_key_amount = kscan_muxes_get_key_amount,
    .get_idx_offset = kscan_muxes_get_idx_offset,
    .get_values = kscan_muxes_get_values,
};

static int kscan_muxes_init(const struct device *dev) {
    const struct kscan_muxes_config *cfg = dev->config;
    struct kscan_muxes_data *data = dev->data;
    int err;

    int total_amount = 0;

    for (uint16_t i = 0; i < cfg->muxes_count; ++i) {
        const struct device *mux = cfg->muxes[i];
        if (!device_is_ready(mux)) {
            LOG_ERR("MUX '%s' is not ready", mux->name);
            return -ENODEV;
        }
        int amount = mux_get_channel_amount(mux);
        if (amount < 0) {
            LOG_ERR("Unable to get MUX '%s' channel amount", mux->name);
            return -ENODEV;
        }
        total_amount += amount;
        err = mux_enable(mux);
        if (err) {
            LOG_ERR("Could not enable MUX '%s' (%d)", mux->name, err);
            return -ENODEV;
        }
        LOG_DBG("MUX '%s' is ready", mux->name);
    }
    if (total_amount != cfg->key_amount) {
        LOG_ERR("Total amount of muxes channels is not the same as key amount");
        return -ENODEV;
    }

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

    LOG_INF("KScan (MUXes) ready: %u MUXes", cfg->muxes_count);

    for (uint16_t i = 0; i < cfg->muxes_count; ++i) {
        k_thread_create(&data->threads[i], data->stacks[i],
                        CONFIG_KSCAN_MUXES_THREAD_STACK_SIZE,
                        kscan_muxes_thread, (void *)dev, &data->chan_idxs[i],
                        NULL, CONFIG_KSCAN_MUXES_THREAD_PRIORITY, 0, K_NO_WAIT);

        k_thread_name_set(&data->threads[i], "kscan_mux");
    }

    return 0;
}

#define MUX_CHANNELS_ELEM(node_id, prop, idx)                                  \
    DT_PROP_OR(DT_PHANDLE_BY_IDX(node_id, prop, idx), channels, 16)

#define KSCAN_MUXES_CHANNELS_SUM(inst)                                         \
    DT_INST_FOREACH_PROP_ELEM_SEP(inst, muxes, MUX_CHANNELS_ELEM, (+))

#define ADC_SPEC_AND_COMMA(node_id, prop, idx)                                 \
    ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

#define MUX_DEV_AND_COMMA(node_id, prop, idx)                                  \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define MUX_IDX_AND_COMMA(node_id, prop, idx) idx,

#define U16_PROP_ELEM_AND_COMMA(node_id, prop, idx)                            \
    DT_PROP_BY_IDX(node_id, prop, idx),

#define THREAD_STACK_NAME(node_id, idx) __kscan_muxes_stack__##node_id##_##idx

#define THREAD_STACK_REFERENCE_IDX_AND_COMMA(node_id, prop, idx)               \
    __kscan_muxes_stack__##node_id##_##idx,

#define THREAD_STACK_DEFINE_IDX(node_id, prop, idx)                            \
    K_THREAD_STACK_DEFINE(THREAD_STACK_NAME(node_id, idx),                     \
                          CONFIG_KSCAN_MUXES_THREAD_STACK_SIZE);

#define ASSERT_SETTLE_TIME_IS_GREATER_THAN_0(inst)                             \
    BUILD_ASSERT((uint32_t)DT_INST_PROP_LEN(inst, settle_time) > 0,            \
                 "settle-time must be greater than 0");

#define KSCAN_MUXES_DEFINE(inst)                                               \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, io_channels) ==                        \
                     DT_INST_PROP_LEN(inst, muxes),                            \
                 "io-channels and muxes must have same length");               \
    static const struct adc_dt_spec __kscan_muxes_adc_channels__##inst[] = {   \
                                                                               \
        DT_INST_FOREACH_PROP_ELEM(inst, io_channels, ADC_SPEC_AND_COMMA)};     \
                                                                               \
    static const struct device *__kscan_muxes_muxes__##inst[] = {              \
        DT_INST_FOREACH_PROP_ELEM(inst, muxes, MUX_DEV_AND_COMMA)};            \
                                                                               \
    DT_INST_FOREACH_PROP_ELEM(inst, muxes, THREAD_STACK_DEFINE_IDX);           \
                                                                               \
    enum { __kscan_muxes_cnt__##inst = DT_INST_PROP_LEN(inst, muxes) };        \
    static struct k_thread                                                     \
        __kscan_muxes_threads__##inst[__kscan_muxes_cnt__##inst] = {};         \
    static uint16_t                                                            \
        __kscan_muxes_chan_idxs__##inst[__kscan_muxes_cnt__##inst] = {         \
            DT_INST_FOREACH_PROP_ELEM(inst, muxes, MUX_IDX_AND_COMMA)};        \
    static k_thread_stack_t                                                    \
        *__kscan_muxes_stacks__##inst[__kscan_muxes_cnt__##inst] = {           \
            DT_INST_FOREACH_PROP_ELEM(inst, muxes,                             \
                                      THREAD_STACK_REFERENCE_IDX_AND_COMMA)};  \
    static uint16_t                                                            \
        __kscan_muxes_values__##inst[KSCAN_MUXES_CHANNELS_SUM(inst)] = {0};    \
                                                                               \
    static const struct kscan_muxes_config __kscan_muxes_config__##inst = {    \
        .channels = __kscan_muxes_adc_channels__##inst,                        \
        .channels_count = DT_INST_PROP_LEN(inst, io_channels),                 \
                                                                               \
        .muxes = (const struct device **)__kscan_muxes_muxes__##inst,          \
        .muxes_count = DT_INST_PROP_LEN(inst, muxes),                          \
                                                                               \
        .idx_offset = DT_INST_PROP(inst, idx_offset),                          \
        .key_amount = (uint16_t)(KSCAN_MUXES_CHANNELS_SUM(inst)),              \
                                                                               \
        .settle_us = DT_INST_PROP(inst, settle_us),                            \
    };                                                                         \
    static struct kscan_muxes_data __kscan_muxes_data__##inst = {              \
        .values = __kscan_muxes_values__##inst,                                \
                                                                               \
        .threads = __kscan_muxes_threads__##inst,                              \
        .stacks = __kscan_muxes_stacks__##inst,                                \
        .chan_idxs = __kscan_muxes_chan_idxs__##inst,                          \
    };                                                                         \
                                                                               \
    DEVICE_DT_INST_DEFINE(                                                     \
        inst, kscan_muxes_init, NULL, &__kscan_muxes_data__##inst,             \
        &__kscan_muxes_config__##inst, POST_KERNEL,                            \
        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &kscan_muxes_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_MUXES_DEFINE)
