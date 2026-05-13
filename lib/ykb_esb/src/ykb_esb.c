#include <lib/ykb_esb.h>

#if CONFIG_LIB_YKB_ESB_MPSL
#include <lib/ykb_timeslot.h>
#endif

#include <esb.h>

#include <stdint.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ykb_esb, CONFIG_YKB_ESB_LOG_LEVEL);

/* ------------------------- App-facing state ------------------------- */

static ykb_esb_callback_t m_callback;
static ykb_esb_event_t m_event;
static ykb_esb_config_t m_config;

static bool m_active;
static uint8_t m_base_addr_0[4];
static uint8_t m_base_addr_1[4];

/* ------------------------- PTX TX queue ---------------------------- */
/*
 * The queue holds one in-flight packet per slot.  K_MSEC(100) in ykb_esb_send
 * lets ESB TX_SUCCESS events drain the queue if it fills during a burst, so 8
 * slots are sufficient without needing a larger (RAM-expensive) buffer.
 */
K_MSGQ_DEFINE(m_msgq_tx_payloads, sizeof(struct esb_payload), 8, 4);

/* ------------------------- RX buffer ------------------------------- */
static struct esb_payload rx_payload;

/* ------------------------- PRX ACK cache (Pattern A) --------------- */

static struct esb_payload m_prx_ack_cache;
static bool m_prx_ack_cache_valid;

/* ------------------------- Forward decls --------------------------- */

static int clocks_start(void);
static int esb_initialize(ykb_esb_mode_t mode);
static int ptx_kick_next_from_msgq(void);

#if CONFIG_LIB_YKB_ESB_MPSL
static void on_timeslot_start_stop(timeslot_callback_type_t type);
#endif

static void prx_preload_cached_ack(void);
static void prx_set_cached_ack_from_data(const uint8_t *data, size_t len);

/* ------------------------- ESB event handler ----------------------- */

static void event_handler(struct esb_evt const *event) {
    static struct esb_payload tmp_payload;

    switch (event->evt_id) {

    case ESB_EVENT_TX_SUCCESS:
        LOG_DBG("ESB_EVENT_TX_SUCCESS");

        /* PTX: consume the item we previously peeked */
        if (m_config.mode == YKB_ESB_MODE_PTX) {
            (void)k_msgq_get(&m_msgq_tx_payloads, &tmp_payload, K_NO_WAIT);
        }

        /* Many ESB implementations deliver ACK payload to RX FIFO after TX. */
        while (esb_read_rx_payload(&rx_payload) == 0) {
            m_event.evt_type = YKB_ESB_EVT_RX;
            m_event.buf = rx_payload.data;
            m_event.data_length = rx_payload.length;
            m_callback(&m_event, m_config.user_ptr);
        }

        m_event.evt_type = YKB_ESB_EVT_TX_SUCCESS;
        m_event.data_length = 0;
        m_callback(&m_event, m_config.user_ptr);

        /* PTX: kick next queued payload now that ESB is idle */
        if (m_config.mode == YKB_ESB_MODE_PTX) {
            (void)ptx_kick_next_from_msgq();
        }
        break;

    case ESB_EVENT_TX_FAILED:
        LOG_DBG("ESB_EVENT_TX_FAILED");

        if (m_config.mode == YKB_ESB_MODE_PTX) {
            esb_flush_tx();
#if CONFIG_LIB_YKB_ESB_MPSL
            /* Within a timeslot: retry immediately — use all available airtime. */
            (void)ptx_kick_next_from_msgq();
#else
            /* Standalone: discard the failed packet so the queue drains.
             * The caller (e.g. alive_work) will re-enqueue after its delay.
             * Immediately retrying here without a PRX floods the IPC event
             * path back to cpuapp and triggers the nRF RPC error handler. */
            (void)k_msgq_get(&m_msgq_tx_payloads, &tmp_payload, K_NO_WAIT);
            (void)ptx_kick_next_from_msgq();
#endif
        }

        m_event.evt_type = YKB_ESB_EVT_TX_FAIL;
        m_event.data_length = 0;
        m_callback(&m_event, m_config.user_ptr);
        break;

    case ESB_EVENT_RX_RECEIVED:
        while (esb_read_rx_payload(&rx_payload) == 0) {
            LOG_DBG("RX len=%u", rx_payload.length);

            m_event.evt_type = YKB_ESB_EVT_RX;
            m_event.buf = rx_payload.data;
            m_event.data_length = rx_payload.length;
            m_callback(&m_event, m_config.user_ptr);
        }
        break;

    default:
        break;
    }
}

/* ------------------------- Clock start ----------------------------- */

static int clocks_start(void) {
    int err;
    int res;
    struct onoff_manager *clk_mgr;
    struct onoff_client clk_cli;

    clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!clk_mgr) {
        LOG_ERR("Unable to get HF clock manager");
        return -ENXIO;
    }

    sys_notify_init_spinwait(&clk_cli.notify);

    err = onoff_request(clk_mgr, &clk_cli);
    if (err < 0) {
        LOG_ERR("Clock request failed: %d", err);
        return err;
    }

    do {
        err = sys_notify_fetch_result(&clk_cli.notify, &res);
        if (!err && res) {
            LOG_ERR("Clock could not be started: %d", res);
            return res;
        }
    } while (err);

    LOG_DBG("HF clock started");
    return 0;
}

/* -------------------- PRX cached ACK payload helpers ---------------- */

static void prx_preload_cached_ack(void) {
    if (m_config.mode != YKB_ESB_MODE_PRX) {
        return;
    }
    if (!m_prx_ack_cache_valid) {
        return;
    }

    /* "Latest wins": clear ESB TX FIFO and preload one payload. */
    esb_flush_tx();

    int ret = esb_write_payload(&m_prx_ack_cache);
    LOG_DBG("PRX preload ack: ret=%d len=%u", ret, m_prx_ack_cache.length);
}

static void prx_set_cached_ack_from_data(const uint8_t *data, size_t len) {
    if (m_config.mode != YKB_ESB_MODE_PRX) {
        return;
    }

    if (!data || len > sizeof(m_prx_ack_cache.data)) {
        return;
    }

    m_prx_ack_cache.pipe = 0;
    m_prx_ack_cache.noack = false;
    m_prx_ack_cache.length = (uint8_t)len;
    memcpy(m_prx_ack_cache.data, data, len);
    m_prx_ack_cache_valid = true;

    if (m_active) {
        prx_preload_cached_ack();
    }
}

int ykb_esb_prx_set_ack_payload(const uint8_t *data, size_t len) {
    if (m_config.mode != YKB_ESB_MODE_PRX) {
        return -EINVAL;
    }
    if (!data || len > sizeof(m_prx_ack_cache.data)) {
        return -EMSGSIZE;
    }

    prx_set_cached_ack_from_data(data, len);
    return 0;
}

/* ------------------------- ESB init -------------------------------- */

static int esb_initialize(ykb_esb_mode_t mode) {
    int err;

    static const uint8_t addr_prefix[8] = {0xE7, 0xC2, 0xC3, 0xC4,
                                           0xC5, 0xC6, 0xC7, 0xC8};

    struct esb_config config = ESB_DEFAULT_CONFIG;

    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.crc = ESB_CRC_8BIT;
    config.retransmit_delay = 600;
    config.retransmit_count = 1;
    config.bitrate = ESB_BITRATE_2MBPS;
    config.event_handler = event_handler;
    config.mode = (mode == YKB_ESB_MODE_PTX) ? ESB_MODE_PTX : ESB_MODE_PRX;
    config.tx_mode = ESB_TXMODE_MANUAL_START;
    config.selective_auto_ack = false;
#if CONFIG_LIB_YKB_ESB_FAST_RAMP_UP
    config.use_fast_ramp_up = true;
#endif

    err = esb_init(&config);
    if (err) {
        LOG_ERR("esb_init err=%d", err);
        return err;
    }

    err = esb_set_base_address_0(m_base_addr_0);
    if (err)
        return err;

    err = esb_set_base_address_1(m_base_addr_1);
    if (err)
        return err;

    err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
    if (err)
        return err;

    /* Note: do NOT override NVIC_SetPriority for RADIO_IRQn here –
     * MPSL manages the RADIO IRQ priority and it must not be changed. */

    if (mode == YKB_ESB_MODE_PRX) {
        /* Preload cached ACK payload before starting RX. */
        prx_preload_cached_ack();
        esb_start_rx();
    }

    return 0;
}

/* -------------------- PTX msgq -> ESB TX ---------------------------- */

/*
 * Peek the next packet from the TX queue, write it to the ESB TX FIFO, and
 * start the TX (if ESB is idle).  Only call when ESB is known to be idle
 * (from resume or from TX_SUCCESS / TX_FAILED handlers).
 *
 * The item is NOT consumed from the queue here – it is removed in the
 * ESB_EVENT_TX_SUCCESS handler after a successful transmission.
 */
static int ptx_kick_next_from_msgq(void) {
    int ret;
    static struct esb_payload tx_payload;

    if (k_msgq_peek(&m_msgq_tx_payloads, &tx_payload) != 0) {
        return -ENOMEM;
    }

    ret = esb_write_payload(&tx_payload);
    if (ret < 0) {
        return ret;
    }

    esb_start_tx();
    return 0;
}

/* ------------------------- Public API ------------------------------ */

int ykb_esb_init(ykb_esb_config_t *cfg, ykb_esb_callback_t callback) {

    m_callback = callback;
    memcpy(&m_config, cfg, sizeof(m_config));
    memcpy(m_base_addr_0, cfg->base_addr_0, sizeof(m_base_addr_0));
    memcpy(m_base_addr_1, cfg->base_addr_1, sizeof(m_base_addr_1));
    m_active = false;

    int ret = clocks_start();
    if (ret < 0) {
        return ret;
    }

#if CONFIG_LIB_YKB_ESB_MPSL
    LOG_INF("Timeslot handler init");
    timeslot_handler_init(on_timeslot_start_stop);
#else
    ret = esb_initialize(cfg->mode);
    if (ret < 0) {
        return ret;
    }
    m_active = true;
    if (cfg->mode == YKB_ESB_MODE_PTX) {
        (void)ptx_kick_next_from_msgq();
    }
#endif

    return 0;
}

/*
 * ykb_esb_send():
 *  - PTX: queue packet for transmission. Kicks the TX pipeline if ESB is
 *    currently idle (i.e. the pipeline has drained).  If ESB is busy the
 *    TX_SUCCESS handler will pick up the next item automatically.
 *  - PRX (MPSL): update the "latest ACK payload" cache.
 */
int ykb_esb_send(ykb_esb_data_t *tx_packet) {
    if (!tx_packet) {
        return -EINVAL;
    }

    if (tx_packet->len > sizeof(((struct esb_payload *)0)->data)) {
        return -EMSGSIZE;
    }

    if (m_config.mode == YKB_ESB_MODE_PRX) {
        prx_set_cached_ack_from_data(tx_packet->data, tx_packet->len);
        return 0;
    }

    /* PTX path */
    struct esb_payload tx_payload;
    tx_payload.pipe = 0;
    tx_payload.noack = false;
    tx_payload.length = tx_packet->len;
    memcpy(tx_payload.data, tx_packet->data, tx_packet->len);

    /*
     * Always wait up to 100 ms for a queue slot.  When ESB is active the
     * TX_SUCCESS handler drains slots at ~1 ms each, so this rarely blocks
     * more than a few ms.  When ESB is suspended (between MPSL timeslots)
     * the queue was purged on suspend, so it is usually empty; if it fills
     * before a slot opens, the next timeslot will drain it well within 100 ms.
     * Using K_NO_WAIT when inactive caused the alive packet to fail every
     * time the queue was refilled by the kscan during an inter-timeslot gap.
     */
    int ret = k_msgq_put(&m_msgq_tx_payloads, &tx_payload, K_MSEC(100));
    if (ret != 0) {
        return -ENOMEM;
    }

    /*
     * Only kick TX if ESB is currently idle.  If it is busy the running
     * TX chain will pick up this packet after the current one completes
     * (in the TX_SUCCESS handler), so double-kicking must be avoided to
     * prevent duplicate entries in the ESB TX FIFO.
     */
    if (m_active && esb_is_idle()) {
        (void)ptx_kick_next_from_msgq();
    }

    return 0;
}

/* ------------------------- Suspend/Resume --------------------------- */

#if CONFIG_LIB_YKB_ESB_MPSL
static int ykb_esb_suspend(void) {
    m_active = false;

    /*
     * esb_disable() disables IRQs, stops the radio, deinitialises PPI and
     * the system timer, and sets ESB state to UNINITIALIZED.  This is safe
     * to call from the MPSL timeslot callback (TIMER0 IRQ context).
     */
    esb_disable();

    /* Clear any stale RADIO pending IRQ so it does not fire after we return. */
    NVIC_ClearPendingIRQ(RADIO_IRQn);

    return 0;
}

static int ykb_esb_resume(void) {
    int err;

    err = esb_initialize(m_config.mode);
    m_active = (err == 0);

    if (err) {
        return err;
    }

    /* PTX: kick TX if anything queued */
    if (m_config.mode == YKB_ESB_MODE_PTX) {
        (void)ptx_kick_next_from_msgq();
    }
    /* PRX: prx_preload_cached_ack() is already called inside esb_initialize() */

    return 0;
}

/* ------------------------- Timeslot callback ------------------------ */

static void on_timeslot_start_stop(timeslot_callback_type_t type) {
    switch (type) {
    case APP_TS_STARTED:
        (void)ykb_esb_resume();
        break;

    case APP_TS_STOPPED:
        (void)ykb_esb_suspend();
        break;

    default:
        break;
    }
}

#endif /* CONFIG_LIB_YKB_ESB_MPSL */
