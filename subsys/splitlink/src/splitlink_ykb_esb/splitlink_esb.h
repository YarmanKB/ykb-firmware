#ifndef SPLITLINK_ESB_H
#define SPLITLINK_ESB_H

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <subsys/splitlink.h>

#include <lib/ykb_esb.h>

#include "generated_splitlink_address.h"

struct splitlink_config {
    const uint8_t esb_default_address[8];
};

struct delayable_work_ctx {
    struct k_work_delayable d_work;
};

struct work_ctx {
    struct k_work work;
};

struct receiving_work_ctx {
    struct k_work work;
    uint8_t data[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
    uint16_t data_len;
};

struct splitlink_data {
#if CONFIG_SPLITLINK_YKB_ESB_PTX
    struct delayable_device_work alive_work;
#endif // CONFIG_SPLITLINK_YKB_ESB_PTX

    bool connected;
    bool ready;

    struct delayable_work_ctx init_work;
    struct work_ctx connect_work;
    struct delayable_work_ctx disconnect_work;
    struct receiving_work_ctx receiving_work;
};

#define FLAG_ALIVE 0U
#define FLAG_DATA 1U

#endif // SPLITLINK_ESB_H
