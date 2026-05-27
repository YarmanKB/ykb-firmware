#ifndef SUBSYS_YKB_METRICS_H
#define SUBSYS_YKB_METRICS_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <stdbool.h>

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

#include <stdint.h>

#define YKB_METRICS_KSCAN_SAMPLE(dev, unit_idx, key_idx, value, resolution)    \
    ykb_metrics_kscan_sample(dev, unit_idx, key_idx, value, resolution)
#define YKB_METRICS_KSCAN_READ_ERROR(dev, unit_idx)                            \
    ykb_metrics_kscan_read_error(dev, unit_idx)
#define YKB_METRICS_KSCAN_SCAN_DONE(dev, unit_idx, samples, elapsed_cycles)    \
    ykb_metrics_kscan_scan_done(dev, unit_idx, samples, elapsed_cycles)
#define YKB_METRICS_KB_MSGQ_PUT(type, err, used)                               \
    ykb_metrics_kb_msgq_put(type, err, used)
#define YKB_METRICS_KB_TRANSITION(pressed) ykb_metrics_kb_transition(pressed)
#define YKB_METRICS_REPORT_SENT(type) ykb_metrics_report_sent(type)

void ykb_metrics_kscan_sample(const struct device *dev, uint16_t unit_idx,
                              uint16_t key_idx, uint16_t value,
                              uint8_t resolution);
void ykb_metrics_kscan_read_error(const struct device *dev, uint16_t unit_idx);
void ykb_metrics_kscan_scan_done(const struct device *dev, uint16_t unit_idx,
                                 uint16_t samples, uint32_t elapsed_cycles);
void ykb_metrics_kb_msgq_put(enum ykb_metrics_kb_msg_type type, int err,
                             uint32_t used);
void ykb_metrics_kb_transition(bool pressed);
void ykb_metrics_report_sent(enum ykb_metrics_report_type type);

#else

#define YKB_METRICS_KSCAN_SAMPLE(dev, unit_idx, key_idx, value, resolution)    \
    (void)(dev);                                                               \
    (void)(unit_idx);                                                          \
    (void)(key_idx);                                                           \
    (void)(value);                                                             \
    (void)(resolution)
#define YKB_METRICS_KSCAN_READ_ERROR(dev, unit_idx)                            \
    (void)(dev);                                                               \
    (void)(unit_idx)
#define YKB_METRICS_KSCAN_SCAN_DONE(dev, unit_idx, samples, elapsed_cycles)    \
    (void)(dev);                                                               \
    (void)(unit_idx);                                                          \
    (void)(samples);                                                           \
    (void)(elapsed_cycles)
#define YKB_METRICS_KB_MSGQ_PUT(type, err, used)                               \
    (void)(type);                                                              \
    (void)(err);                                                               \
    (void)(used)
#define YKB_METRICS_KB_TRANSITION(pressed) (void)(pressed)
#define YKB_METRICS_REPORT_SENT(type) (void)(type)

#endif // CONFIG_YKB_METRICS

#endif // SUBSYS_YKB_METRICS_H
