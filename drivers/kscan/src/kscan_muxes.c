#define DT_DRV_COMPAT kscan_muxes

#include <drivers/kscan.h>

#include <drivers/mux.h>
#include <subsys/ykb_metrics.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/devicetree/io-channels.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

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
    struct adc_sequence adc_seq;
    struct adc_sequence_options adc_seq_opts;
    uint16_t *read_buf;
};

static int kscan_muxes_get_key_amount(const struct device *dev) {
    const struct kscan_muxes_config *cfg = dev->config;
    return cfg->key_amount;
}

static int kscan_muxes_get_idx_offset(const struct device *dev) {
    const struct kscan_muxes_config *cfg = dev->config;
    return cfg->idx_offset;
}

static void kscan_muxes_prepare_sequence(struct kscan_muxes_data *data,
                                         const struct adc_dt_spec *adc_spec,
                                         uint16_t *buffer, size_t buffer_size) {
    data->adc_seq.buffer = buffer;
    data->adc_seq.buffer_size = buffer_size;
    data->adc_seq.resolution = adc_spec->resolution;
    data->adc_seq.options = &data->adc_seq_opts;
    data->adc_seq.oversampling = adc_spec->oversampling;
    data->adc_seq.calibrate = false;
    data->adc_seq.channels = BIT(adc_spec->channel_id);
}

#if CONFIG_KSCAN_MUXES_PARALLEL_ADC_READS
static int kscan_muxes_parallel_scan(const struct device *dev, uint16_t *values,
                                     const int *channel_amount,
                                     const uint16_t *mux_offsets,
                                     int max_channel_amount) {
    const struct kscan_muxes_config *cfg = dev->config;
    struct kscan_muxes_data *data = dev->data;
    uint16_t samples = 0U;
    uint32_t start_cycles = k_cycle_get_32();
    int ret = 0;

    for (size_t i = 0; i < max_channel_amount; ++i) {
        for (size_t j = 0; j < cfg->muxes_count; ++j) {
            const struct device *mux = cfg->muxes[j];

            if (i >= channel_amount[j]) {
                continue;
            }

            ret = mux_select(mux, i);
            if (ret) {
                LOG_ERR("mux_select: %d", ret);
                return ret;
            }
        }

        if (cfg->settle_us > 0U) {
            k_busy_wait(cfg->settle_us);
        }

        ret = adc_read(cfg->channels[0].dev, &data->adc_seq);
        if (ret) {
            LOG_ERR("ADC read: %d", ret);
            YKB_METRICS_KSCAN_READ_ERROR(dev, 0U);
            return ret;
        }

        for (size_t j = 0; j < cfg->muxes_count; ++j) {
            uint16_t key_idx;
            uint16_t value;

            if (i >= channel_amount[j]) {
                continue;
            }

            key_idx = mux_offsets[j] + (uint16_t)i;
            value = data->read_buf[j];
            values[key_idx] = value;
            YKB_METRICS_KSCAN_SAMPLE(dev, 0U, cfg->idx_offset + key_idx, value,
                                     data->adc_seq.resolution);
            samples++;
        }
    }

    YKB_METRICS_KSCAN_SCAN_DONE(dev, 0U, samples,
                                k_cycle_get_32() - start_cycles);

    return 0;
}
#else
static int kscan_muxes_stable_scan(const struct device *dev, uint16_t *values,
                                   const int *channel_amount,
                                   const uint16_t *mux_offsets) {
    const struct kscan_muxes_config *cfg = dev->config;
    struct kscan_muxes_data *data = dev->data;

    for (size_t mux_idx = 0; mux_idx < cfg->muxes_count; ++mux_idx) {
        const struct device *mux = cfg->muxes[mux_idx];
        const struct adc_dt_spec *adc_spec = &cfg->channels[mux_idx];
        uint32_t start_cycles = k_cycle_get_32();
        uint16_t samples = 0U;

        for (size_t i = 0; i < channel_amount[mux_idx]; ++i) {
            uint16_t key_idx;
            uint16_t value;
            int ret;

            ret = mux_select(mux, i);
            if (ret) {
                LOG_ERR("mux_select: %d", ret);
                return ret;
            }

            if (cfg->settle_us > 0U) {
                k_busy_wait(cfg->settle_us);
            }

            kscan_muxes_prepare_sequence(data, adc_spec,
                                         &data->read_buf[mux_idx],
                                         sizeof(data->read_buf[mux_idx]));
            ret = adc_read(adc_spec->dev, &data->adc_seq);
            if (ret) {
                LOG_ERR("ADC read: %d", ret);
                YKB_METRICS_KSCAN_READ_ERROR(dev, mux_idx);
                return ret;
            }

            key_idx = mux_offsets[mux_idx] + (uint16_t)i;
            value = data->read_buf[mux_idx];
            values[key_idx] = value;
            YKB_METRICS_KSCAN_SAMPLE(dev, mux_idx, cfg->idx_offset + key_idx,
                                     value, adc_spec->resolution);
            samples++;
        }

        YKB_METRICS_KSCAN_SCAN_DONE(dev, mux_idx, samples,
                                    k_cycle_get_32() - start_cycles);
    }

    return 0;
}
#endif

static int kscan_muxes_scan(const struct device *dev, uint16_t *values) {
    if (!values) {
        return -EINVAL;
    }
    const struct kscan_muxes_config *cfg = dev->config;

    int channel_amount[cfg->muxes_count];
    uint16_t mux_offsets[cfg->muxes_count];
    int max_channel_amount = 0;
    uint16_t next_offset = 0;
    int ret = 0;

    for (size_t i = 0; i < cfg->muxes_count; ++i) {
        const struct device *mux = cfg->muxes[i];
        int amount = mux_get_channel_amount(mux);
        if (amount < 0) {
            LOG_ERR("mux_get_channel_amount: %d", amount);
            return -EINVAL;
        }
        channel_amount[i] = amount;
        mux_offsets[i] = next_offset;
        next_offset += (uint16_t)amount;
        max_channel_amount =
            amount > max_channel_amount ? amount : max_channel_amount;
#if CONFIG_KSCAN_MUXES_MUX_TOGGLE
        ret = mux_enable(mux);
        if (ret) {
            LOG_ERR("mux_enable: %d", ret);
            goto cleanup;
        }
#endif // CONFIG_KSCAN_MUXES_MUX_TOGGLE
    }

#if CONFIG_KSCAN_MUXES_PARALLEL_ADC_READS
    ret = kscan_muxes_parallel_scan(dev, values, channel_amount, mux_offsets,
                                    max_channel_amount);
#else
    ret = kscan_muxes_stable_scan(dev, values, channel_amount, mux_offsets);
#endif

#if CONFIG_KSCAN_MUXES_MUX_TOGGLE
cleanup:
    for (size_t i = 0; i < cfg->muxes_count; ++i) {
        const struct device *mux = cfg->muxes[i];
        int err = mux_disable(mux);
        if (err) {
            LOG_WRN("mux_disable: %d", err);
        }
    }
#endif // CONFIG_KSCNA_MUXES_MUX_TOGGLE

    return ret;
}

DEVICE_API(kscan, kscan_muxes_api) = {
    .get_key_amount = kscan_muxes_get_key_amount,
    .get_idx_offset = kscan_muxes_get_idx_offset,
    .scan = kscan_muxes_scan,
};

static int kscan_muxes_init(const struct device *dev) {
    const struct kscan_muxes_config *cfg = dev->config;
    struct kscan_muxes_data *data = dev->data;

    ARG_UNUSED(data);

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
#if !CONFIG_KSCAN_MUXES_MUX_TOGGLE
        int err = mux_enable(mux);
        if (err) {
            LOG_ERR("Could not enable MUX '%s' (%d)", mux->name, err);
            return -ENODEV;
        }
#endif // !CONFIG_KSCAN_MUXES_MUX_TOGGLE
        LOG_DBG("MUX '%s' is ready", mux->name);
    }
    if (total_amount != cfg->key_amount) {
        LOG_ERR("Total amount of muxes channels is not the same as key amount");
        return -ENODEV;
    }

    data->adc_seq_opts.interval_us = 0;
    data->adc_seq_opts.callback = NULL;
    data->adc_seq_opts.extra_samplings = 0;

    for (uint16_t i = 0; i < cfg->channels_count; ++i) {
        const struct adc_dt_spec *adc_spec = &cfg->channels[i];
#if CONFIG_KSCAN_MUXES_PARALLEL_ADC_READS
        if (adc_spec->dev != cfg->channels[0].dev ||
            adc_spec->resolution != cfg->channels[0].resolution ||
            adc_spec->oversampling != cfg->channels[0].oversampling) {
            LOG_ERR("All ADC channels in one kscan-muxes device must use the "
                    "same ADC device, resolution, and oversampling");
            return -ENODEV;
        }
        if (i > 0U &&
            adc_spec->channel_id <= cfg->channels[i - 1U].channel_id) {
            LOG_ERR("ADC channels must be listed in ascending channel order");
            return -ENODEV;
        }
#endif
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

#if CONFIG_KSCAN_MUXES_PARALLEL_ADC_READS
    kscan_muxes_prepare_sequence(data, &cfg->channels[0], data->read_buf,
                                 sizeof(uint16_t) * cfg->channels_count);
    data->adc_seq.channels = 0;
    for (uint16_t i = 0; i < cfg->channels_count; ++i) {
        data->adc_seq.channels |= BIT(cfg->channels[i].channel_id);
    }

    if (cfg->channels_count > 1U && data->adc_seq.oversampling > 0U) {
        LOG_WRN("Disabling ADC oversampling for multi-channel kscan scan");
        data->adc_seq.oversampling = 0U;
    }
#endif

    LOG_INF("KScan (MUXes) ready: %u MUXes", cfg->muxes_count);

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

#define ASSERT_SETTLE_TIME_IS_GREATER_THAN_0(inst)                             \
    BUILD_ASSERT((uint32_t)DT_INST_PROP_LEN(inst, settle_time) > 0,            \
                 "settle-time must be greater than 0");

#define KSCAN_MUXES_DEFINE(inst)                                               \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, io_channels) > 0,                      \
                 "io-channels must not be empty");                             \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, muxes) > 0,                            \
                 "muxes must not be empty");                                   \
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
    static uint16_t __kscan_muxes_read_buf__##inst[DT_INST_PROP_LEN(           \
        inst, io_channels)] = {0};                                             \
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
        .read_buf = __kscan_muxes_read_buf__##inst,                            \
    };                                                                         \
                                                                               \
    DEVICE_DT_INST_DEFINE(                                                     \
        inst, kscan_muxes_init, NULL, &__kscan_muxes_data__##inst,             \
        &__kscan_muxes_config__##inst, POST_KERNEL,                            \
        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &kscan_muxes_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_MUXES_DEFINE)
