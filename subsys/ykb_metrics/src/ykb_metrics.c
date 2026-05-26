#include <subsys/ykb_metrics.h>

#include <subsys/kb_settings.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ykb_metrics, CONFIG_YKB_METRICS_LOG_LEVEL);

struct kscan_unit_metrics {
    const struct device *dev;
    uint16_t unit_idx;
    atomic_t registered;
    atomic_t samples;
    atomic_t scans;
    atomic_t read_errors;
    atomic_t press_events;
    atomic_t release_events;
    atomic_t samples_per_scan_max;
    atomic_t scan_us_total;
    atomic_t scan_us_max;
};

struct key_metrics {
    atomic_t seen;
    atomic_t current;
    atomic_t min;
    atomic_t max;
    atomic_t clip_count;
};

struct kb_msgq_metrics {
    atomic_t puts[YKB_METRICS_KB_MSG_COUNT];
    atomic_t drops[YKB_METRICS_KB_MSG_COUNT];
    atomic_t max_depth;
};

struct report_metrics {
    atomic_t sent[YKB_METRICS_REPORT_COUNT];
};

static struct kscan_unit_metrics kscan_units[CONFIG_YKB_METRICS_KSCAN_UNIT_MAX];
static struct key_metrics keys[CONFIG_KB_SETTINGS_KEY_COUNT];
static struct kb_msgq_metrics kb_msgq;
static struct report_metrics reports;
static struct k_spinlock registry_lock;

static void atomic_max(atomic_t *target, atomic_val_t value) {
    atomic_val_t old = atomic_get(target);

    while (value > old && !atomic_cas(target, old, value)) {
        old = atomic_get(target);
    }
}

static void atomic_min(atomic_t *target, atomic_val_t value) {
    atomic_val_t old = atomic_get(target);

    while (value < old && !atomic_cas(target, old, value)) {
        old = atomic_get(target);
    }
}

static struct kscan_unit_metrics *kscan_unit_get(const struct device *dev,
                                                 uint16_t unit_idx) {
    for (size_t i = 0; i < ARRAY_SIZE(kscan_units); ++i) {
        struct kscan_unit_metrics *unit = &kscan_units[i];

        if (atomic_get(&unit->registered) && unit->dev == dev &&
            unit->unit_idx == unit_idx) {
            return unit;
        }
    }

    k_spinlock_key_t key = k_spin_lock(&registry_lock);

    for (size_t i = 0; i < ARRAY_SIZE(kscan_units); ++i) {
        struct kscan_unit_metrics *unit = &kscan_units[i];

        if (atomic_get(&unit->registered) && unit->dev == dev &&
            unit->unit_idx == unit_idx) {
            k_spin_unlock(&registry_lock, key);
            return unit;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(kscan_units); ++i) {
        struct kscan_unit_metrics *unit = &kscan_units[i];

        if (!atomic_get(&unit->registered)) {
            unit->dev = dev;
            unit->unit_idx = unit_idx;
            atomic_set(&unit->registered, 1);
            k_spin_unlock(&registry_lock, key);
            return unit;
        }
    }

    k_spin_unlock(&registry_lock, key);
    return NULL;
}

static uint16_t adc_max_for_resolution(uint8_t resolution) {
    if (resolution >= 16U) {
        return UINT16_MAX;
    }

    return (uint16_t)(BIT(resolution) - 1U);
}

void ykb_metrics_kscan_sample(const struct device *dev, uint16_t unit_idx,
                              uint16_t key_idx, uint16_t value,
                              uint8_t resolution) {
    struct kscan_unit_metrics *unit = kscan_unit_get(dev, unit_idx);

    if (unit) {
        atomic_inc(&unit->samples);
    }

    if (key_idx >= ARRAY_SIZE(keys)) {
        return;
    }

    struct key_metrics *key = &keys[key_idx];
    if (!atomic_get(&key->seen)) {
        atomic_set(&key->current, value);
        atomic_set(&key->min, value);
        atomic_set(&key->max, value);
        atomic_set(&key->seen, 1);
    } else {
        atomic_set(&key->current, value);
        atomic_min(&key->min, value);
        atomic_max(&key->max, value);
    }

    if (value >= adc_max_for_resolution(resolution)) {
        atomic_inc(&key->clip_count);
    }
}

void ykb_metrics_kscan_read_error(const struct device *dev, uint16_t unit_idx) {
    struct kscan_unit_metrics *unit = kscan_unit_get(dev, unit_idx);

    if (unit) {
        atomic_inc(&unit->read_errors);
    }
}

void ykb_metrics_kscan_transition(const struct device *dev, uint16_t unit_idx,
                                  bool pressed) {
    struct kscan_unit_metrics *unit = kscan_unit_get(dev, unit_idx);

    if (!unit) {
        return;
    }

    if (pressed) {
        atomic_inc(&unit->press_events);
    } else {
        atomic_inc(&unit->release_events);
    }
}

void ykb_metrics_kscan_scan_done(const struct device *dev, uint16_t unit_idx,
                                 uint16_t samples, uint32_t elapsed_cycles) {
    struct kscan_unit_metrics *unit = kscan_unit_get(dev, unit_idx);

    if (!unit) {
        return;
    }

    atomic_inc(&unit->scans);
    atomic_max(&unit->samples_per_scan_max, samples);
    uint32_t elapsed_us = k_cyc_to_us_floor32(elapsed_cycles);
    atomic_add(&unit->scan_us_total, elapsed_us);
    atomic_max(&unit->scan_us_max, elapsed_us);
}

void ykb_metrics_kb_msgq_put(enum ykb_metrics_kb_msg_type type, int err,
                             uint32_t used) {
    if (type >= YKB_METRICS_KB_MSG_COUNT) {
        return;
    }

    atomic_inc(&kb_msgq.puts[type]);
    if (err) {
        atomic_inc(&kb_msgq.drops[type]);
    }
    atomic_max(&kb_msgq.max_depth, used);
}

void ykb_metrics_report_sent(enum ykb_metrics_report_type type) {
    if (type >= YKB_METRICS_REPORT_COUNT) {
        return;
    }

    atomic_inc(&reports.sent[type]);
}

#if CONFIG_YKB_METRICS_LOG
static atomic_val_t diff_counter(atomic_t *counter, atomic_val_t *previous) {
    atomic_val_t current = atomic_get(counter);
    atomic_val_t diff = current - *previous;

    *previous = current;
    return diff;
}

static void metrics_log_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(metrics_log_work, metrics_log_work_handler);

static void metrics_log_kscan(void) {
    static atomic_val_t prev_samples[CONFIG_YKB_METRICS_KSCAN_UNIT_MAX];
    static atomic_val_t prev_scans[CONFIG_YKB_METRICS_KSCAN_UNIT_MAX];
    static atomic_val_t prev_errors[CONFIG_YKB_METRICS_KSCAN_UNIT_MAX];
    static atomic_val_t prev_scan_us[CONFIG_YKB_METRICS_KSCAN_UNIT_MAX];
    uint32_t interval_ms = CONFIG_YKB_METRICS_LOG_INTERVAL_MS;

    for (size_t i = 0; i < ARRAY_SIZE(kscan_units); ++i) {
        struct kscan_unit_metrics *unit = &kscan_units[i];

        if (!atomic_get(&unit->registered)) {
            continue;
        }

        atomic_val_t samples = diff_counter(&unit->samples, &prev_samples[i]);
        atomic_val_t scans = diff_counter(&unit->scans, &prev_scans[i]);
        atomic_val_t errors = diff_counter(&unit->read_errors, &prev_errors[i]);
        atomic_val_t scan_us =
            diff_counter(&unit->scan_us_total, &prev_scan_us[i]);
        atomic_val_t avg_scan_us = scans > 0 ? scan_us / scans : 0;
        atomic_val_t max_scan_us = atomic_get(&unit->scan_us_max);

        LOG_INF("kscan[%s:%u] samples=%ld/s scans=%ld/s avg=%ldus max=%ldus "
                "errors=%ld/s transitions=%ld/%ld",
                unit->dev ? unit->dev->name : "?", unit->unit_idx,
                (long)((samples * MSEC_PER_SEC) / interval_ms),
                (long)((scans * MSEC_PER_SEC) / interval_ms), (long)avg_scan_us,
                (long)max_scan_us,
                (long)((errors * MSEC_PER_SEC) / interval_ms),
                (long)atomic_get(&unit->press_events),
                (long)atomic_get(&unit->release_events));
    }
}

static void metrics_log_keys(void) {
    atomic_val_t clip_total = 0;
    uint16_t noisiest_key = 0;
    atomic_val_t noisiest_span = 0;

    for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
        struct key_metrics *key = &keys[i];

        if (!atomic_get(&key->seen)) {
            continue;
        }

        atomic_val_t min = atomic_get(&key->min);
        atomic_val_t max = atomic_get(&key->max);
        atomic_val_t span = max - min;

        clip_total += atomic_get(&key->clip_count);
        if (span > noisiest_span) {
            noisiest_span = span;
            noisiest_key = i;
        }
    }

    LOG_INF("adc keys: clips=%ld widest_span=key%u:%ld", (long)clip_total,
            noisiest_key, (long)noisiest_span);
}

static void metrics_log_kb_handler(void) {
    static atomic_val_t prev_puts[YKB_METRICS_KB_MSG_COUNT];
    static atomic_val_t prev_drops[YKB_METRICS_KB_MSG_COUNT];
    uint32_t interval_ms = CONFIG_YKB_METRICS_LOG_INTERVAL_MS;

    LOG_INF("kb msgq: key=%ld/s value=%ld/s slave=%ld/s drops=%ld/%ld/%ld "
            "max_depth=%ld reports=%ld/%ld",
            (long)((diff_counter(&kb_msgq.puts[YKB_METRICS_KB_MSG_KEY],
                                 &prev_puts[YKB_METRICS_KB_MSG_KEY]) *
                    MSEC_PER_SEC) /
                   interval_ms),
            (long)((diff_counter(&kb_msgq.puts[YKB_METRICS_KB_MSG_VALUE],
                                 &prev_puts[YKB_METRICS_KB_MSG_VALUE]) *
                    MSEC_PER_SEC) /
                   interval_ms),
            (long)((diff_counter(&kb_msgq.puts[YKB_METRICS_KB_MSG_SLAVE_VALUES],
                                 &prev_puts[YKB_METRICS_KB_MSG_SLAVE_VALUES]) *
                    MSEC_PER_SEC) /
                   interval_ms),
            (long)diff_counter(&kb_msgq.drops[YKB_METRICS_KB_MSG_KEY],
                               &prev_drops[YKB_METRICS_KB_MSG_KEY]),
            (long)diff_counter(&kb_msgq.drops[YKB_METRICS_KB_MSG_VALUE],
                               &prev_drops[YKB_METRICS_KB_MSG_VALUE]),
            (long)diff_counter(&kb_msgq.drops[YKB_METRICS_KB_MSG_SLAVE_VALUES],
                               &prev_drops[YKB_METRICS_KB_MSG_SLAVE_VALUES]),
            (long)atomic_get(&kb_msgq.max_depth),
            (long)atomic_get(&reports.sent[YKB_METRICS_REPORT_KBD]),
            (long)atomic_get(&reports.sent[YKB_METRICS_REPORT_MOUSE]));
}

static void metrics_log_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    metrics_log_kscan();
    metrics_log_keys();
    metrics_log_kb_handler();

    (void)k_work_reschedule(&metrics_log_work,
                            K_MSEC(CONFIG_YKB_METRICS_LOG_INTERVAL_MS));
}

static int ykb_metrics_init(void) {
    (void)k_work_schedule(&metrics_log_work,
                          K_MSEC(CONFIG_YKB_METRICS_LOG_INTERVAL_MS));
    return 0;
}

SYS_INIT(ykb_metrics_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif // CONFIG_YKB_METRICS_LOG
