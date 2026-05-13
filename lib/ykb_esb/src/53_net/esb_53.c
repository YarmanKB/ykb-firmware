#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/types.h>

#include "../esb_rpc_ids.h"
#include <lib/ykb_esb.h>

#include <esb.h>
#include <mdk/nrf.h>

#include <nrf_rpc/nrf_rpc_ipc.h>
#include <nrf_rpc_cbor.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#define CBOR_BUF_SIZE 16

LOG_MODULE_REGISTER(ykb_esb_rpc, CONFIG_YKB_ESB_LOG_LEVEL);

NRF_RPC_IPC_TRANSPORT(esb_group_tr, DEVICE_DT_GET(DT_NODELABEL(ipc0)),
                      "nrf_rpc_ept");
NRF_RPC_GROUP_DEFINE_NOWAIT(esb_group, "esb_group_id", &esb_group_tr, NULL,
                            NULL, NULL, NULL, false);

static void rpc_esb_event_send(uint32_t evt_type, const uint8_t *rx_payload_buf,
                               uint32_t rx_payload_length);

static void work_send_evt_tx_success_func(struct k_work *item);
static void work_send_evt_tx_fail_func(struct k_work *item);
static void work_send_evt_rx_received_func(struct k_work *item);

K_WORK_DEFINE(m_work_send_evt_tx_success, work_send_evt_tx_success_func);
K_WORK_DEFINE(m_work_send_evt_tx_fail, work_send_evt_tx_fail_func);
K_WORK_DEFINE(m_work_send_evt_rx_received, work_send_evt_rx_received_func);

/*
 * RX data copy buffer.  The ESB event callback runs in ISR context and
 * event->buf points to a module-static buffer that may be overwritten by
 * the next RX event before the k_work item is processed.  Copy the payload
 * here immediately in the callback so the work item always sees stable data.
 */
static uint8_t last_rx_data[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
static uint32_t last_rx_length;
static bool rpc_initialized;

void on_esb_callback(ykb_esb_event_t *event, void *user_data) {
    switch (event->evt_type) {
    case YKB_ESB_EVT_TX_SUCCESS:
        LOG_INF("ESB TX success");
        k_work_submit(&m_work_send_evt_tx_success);
        break;
    case YKB_ESB_EVT_TX_FAIL:
        LOG_INF("ESB TX failed");
        k_work_submit(&m_work_send_evt_tx_fail);
        break;
    case YKB_ESB_EVT_RX:
        LOG_INF("ESB RX: 0x%.2x-0x%.2x-0x%.2x-0x%.2x", event->buf[0],
                event->buf[1], event->buf[2], event->buf[3]);
        /* Copy immediately – event->buf is a static buffer that will be
         * reused for the next RX before the work handler runs. */
        last_rx_length = MIN(event->data_length, sizeof(last_rx_data));
        memcpy(last_rx_data, event->buf, last_rx_length);
        k_work_submit(&m_work_send_evt_rx_received);
        break;
    default:
        LOG_ERR("Unknown APP ESB event!");
        break;
    }
}

static void work_send_evt_tx_success_func(struct k_work *item) {
    rpc_esb_event_send(YKB_ESB_EVT_TX_SUCCESS, NULL, 0);
}

static void work_send_evt_tx_fail_func(struct k_work *item) {
    rpc_esb_event_send(YKB_ESB_EVT_TX_FAIL, NULL, 0);
}

static void work_send_evt_rx_received_func(struct k_work *item) {
    rpc_esb_event_send(YKB_ESB_EVT_RX, last_rx_data, last_rx_length);
}

static int decode_struct(struct nrf_rpc_cbor_ctx *ctx, void *struct_ptr,
                         size_t expected_size) {
    struct zcbor_string zst;
    int err;

    if (zcbor_bstr_decode(ctx->zs, &zst)) {
        err = 0;
    } else {
        err = -EBADMSG;
    }

    if (!err && expected_size != zst.len) {
        LOG_ERR("struct size mismatch: expect %zu got %zu", expected_size,
                zst.len);
        err = -EMSGSIZE;
    }

    if (!err) {
        memcpy(struct_ptr, zst.value, zst.len);
    } else {
        LOG_ERR("decoding failed: %d", err);
    }

    return err;
}

static void rpc_rsp(int32_t err) {
    struct nrf_rpc_cbor_ctx ctx;

    NRF_RPC_CBOR_ALLOC(&esb_group, ctx, CBOR_BUF_SIZE);
    zcbor_int32_put(ctx.zs, err);
    nrf_rpc_cbor_rsp_no_err(&esb_group, &ctx);
}

static void rpc_esb_init_handler(const struct nrf_rpc_group *group,
                                 struct nrf_rpc_cbor_ctx *ctx,
                                 void *handler_data) {
    int32_t err = 0;
    ykb_esb_config_t config;

    if (decode_struct(ctx, &config, sizeof(config))) {
        err = -EBADMSG;
    }

    /* Release the decoding context before performing any blocking work. */
    nrf_rpc_cbor_decoding_done(group, ctx);

    if (!err) {
        LOG_DBG("ykb_esb_init mode=%d", config.mode);
        config.user_ptr = NULL;
        err = ykb_esb_init(&config, on_esb_callback);
        if (err) {
            LOG_ERR("ykb_esb_init failed: %d", err);
        }
    }

    rpc_rsp(err);
}

static void rpc_esb_tx_handler(const struct nrf_rpc_group *group,
                               struct nrf_rpc_cbor_ctx *ctx,
                               void *handler_data) {
    int err = 0;
    ykb_esb_data_t tx_payload;

    if (decode_struct(ctx, &tx_payload, sizeof(tx_payload))) {
        err = -EBADMSG;
    }

    nrf_rpc_cbor_decoding_done(group, ctx);

    if (!err) {
        LOG_DBG("ESB TX pkt data[0]=0x%.2x len=%u", tx_payload.data[0],
                tx_payload.len);
        err = ykb_esb_send(&tx_payload);
        if (err < 0) {
            LOG_ERR("ykb_esb_send: %d", err);
        }
    }

    rpc_rsp(err);
}

static void rpc_esb_event_send(uint32_t evt_type, const uint8_t *rx_buf,
                               uint32_t rx_length) {
    int err = 0;
    struct nrf_rpc_cbor_ctx ctx;

    NRF_RPC_CBOR_ALLOC(&esb_group, ctx,
                       CBOR_BUF_SIZE + sizeof(err) + sizeof(evt_type) +
                           sizeof(uint32_t) + rx_length);

    if (!zcbor_int32_put(ctx.zs, err)) {
        err = -EINVAL;
    }
    if (err || !zcbor_uint32_put(ctx.zs, evt_type)) {
        err = -EINVAL;
    }
    if (err || !zcbor_uint32_put(ctx.zs, rx_length)) {
        err = -EINVAL;
    }
    if (rx_length > 0) {
        if (err || !zcbor_bstr_encode_ptr(ctx.zs, rx_buf, rx_length)) {
            err = -EINVAL;
        }
    }

    if (!err) {
        err = nrf_rpc_cbor_evt(&esb_group, RPC_EVENT_ESB_CB, &ctx);
    }

    if (err) {
        LOG_DBG("rpc_esb_event_send err=%d", err);
    }
}

NRF_RPC_CBOR_CMD_DECODER(esb_group, rpc_esb_init, RPC_COMMAND_ESB_INIT,
                         rpc_esb_init_handler, NULL);
NRF_RPC_CBOR_CMD_DECODER(esb_group, rpc_esb_tx, RPC_COMMAND_ESB_TX,
                         rpc_esb_tx_handler, NULL);

static void err_handler(const struct nrf_rpc_err_report *report) {
    LOG_ERR("nRF RPC error %d", report->code);
    k_oops();
}

int ykb_esb_rpc_start(void) {
    int err;

    if (rpc_initialized) {
        return 0;
    }

    LOG_DBG("nRF RPC init");

    err = nrf_rpc_init(err_handler);
    if (err) {
        return -EINVAL;
    }

    rpc_initialized = true;
    LOG_DBG("nRF RPC init ok");

    return 0;
}
