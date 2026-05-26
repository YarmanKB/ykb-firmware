#ifndef SUBSYS_YKB_METRICS_H
#define SUBSYS_YKB_METRICS_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stdint.h>

enum ykb_metrics_kb_msg_type {
    YKB_METRICS_KB_MSG_KEY = 0U,
    YKB_METRICS_KB_MSG_VALUE,
    YKB_METRICS_KB_MSG_SLAVE_VALUES,
    YKB_METRICS_KB_MSG_SLAVE_RESET,
    YKB_METRICS_KB_MSG_SETTINGS_SYNC,
    YKB_METRICS_KB_MSG_COUNT,
};

enum ykb_metrics_report_type {
    YKB_METRICS_REPORT_KBD = 0U,
    YKB_METRICS_REPORT_MOUSE,
    YKB_METRICS_REPORT_COUNT,
};

#if CONFIG_YKB_METRICS

void ykb_metrics_kscan_sample(const struct device *dev, uint16_t unit_idx,
                              uint16_t key_idx, uint16_t value,
                              uint8_t resolution);
void ykb_metrics_kscan_read_error(const struct device *dev, uint16_t unit_idx);
void ykb_metrics_kscan_transition(const struct device *dev, uint16_t unit_idx,
                                  bool pressed);
void ykb_metrics_kscan_scan_done(const struct device *dev, uint16_t unit_idx,
                                 uint16_t samples, uint32_t elapsed_cycles);
void ykb_metrics_kb_msgq_put(enum ykb_metrics_kb_msg_type type, int err,
                             uint32_t used);
void ykb_metrics_report_sent(enum ykb_metrics_report_type type);

#else

static inline void ykb_metrics_kscan_sample(const struct device *dev,
                                            uint16_t unit_idx, uint16_t key_idx,
                                            uint16_t value,
                                            uint8_t resolution) {
    ARG_UNUSED(dev);
    ARG_UNUSED(unit_idx);
    ARG_UNUSED(key_idx);
    ARG_UNUSED(value);
    ARG_UNUSED(resolution);
}

static inline void ykb_metrics_kscan_read_error(const struct device *dev,
                                                uint16_t unit_idx) {
    ARG_UNUSED(dev);
    ARG_UNUSED(unit_idx);
}

static inline void ykb_metrics_kscan_transition(const struct device *dev,
                                                uint16_t unit_idx,
                                                bool pressed) {
    ARG_UNUSED(dev);
    ARG_UNUSED(unit_idx);
    ARG_UNUSED(pressed);
}

static inline void ykb_metrics_kscan_scan_done(const struct device *dev,
                                               uint16_t unit_idx,
                                               uint16_t samples,
                                               uint32_t elapsed_cycles) {
    ARG_UNUSED(dev);
    ARG_UNUSED(unit_idx);
    ARG_UNUSED(samples);
    ARG_UNUSED(elapsed_cycles);
}

static inline void ykb_metrics_kb_msgq_put(enum ykb_metrics_kb_msg_type type,
                                           int err, uint32_t used) {
    ARG_UNUSED(type);
    ARG_UNUSED(err);
    ARG_UNUSED(used);
}

static inline void
ykb_metrics_report_sent(enum ykb_metrics_report_type type) {
    ARG_UNUSED(type);
}

#endif // CONFIG_YKB_METRICS

#endif // SUBSYS_YKB_METRICS_H
