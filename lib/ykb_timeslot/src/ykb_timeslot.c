#include "mdk/nrf5340_network.h"

#include <lib/ykb_timeslot.h>

#include <hal/nrf_timer.h>

#include <mdk/nrf.h>
#include <mpsl.h>
#include <mpsl_timeslot.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ykb_timeslot, CONFIG_YKB_TIMESLOT_LOG_LEVEL);

/*
 * Timeslot parameters:
 *
 *   LENGTH_US            : initial (and extension) slot duration
 *   TIMER_EXPIRY_US_EARLY: fire CC0 this many µs before slot end to attempt
 *                          an extension; must satisfy the MPSL extension
 * margin. TIMER_EXPIRY_REQ     : fire CC1 this many µs before slot end to
 * request a fresh timeslot if extension failed.
 */
#define TIMESLOT_REQUEST_TIMEOUT_US 1000000
#define TIMESLOT_LENGTH_US 10000
#define TIMESLOT_EXT_MARGIN_US 1000
#define TIMESLOT_REQ_EARLIEST_MARGIN_US 200

#define TIMER_EXPIRY_US_EARLY                                                  \
    (TIMESLOT_LENGTH_US - MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US -              \
     TIMESLOT_EXT_MARGIN_US)
#define TIMER_EXPIRY_REQ                                                       \
    (TIMESLOT_LENGTH_US - MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US -              \
     TIMESLOT_REQ_EARLIEST_MARGIN_US)

/* The MPSL non-preemptible thread must run at this cooperative priority. */
#define MPSL_THREAD_PRIO CONFIG_MPSL_THREAD_COOP_PRIO
#define STACKSIZE CONFIG_LIB_YKB_TIMESLOT_STACK_SIZE

static timeslot_callback_t m_callback;
static volatile bool m_in_timeslot;

/* Declare the ESB RADIO IRQ handler – prevents implicit-function-declaration
 * warning */
void radio_irq_handler(void);

/* Messages to serialise MPSL API calls onto the non-preemptible thread */
enum mpsl_timeslot_call { OPEN_SESSION, MAKE_REQUEST, CLOSE_SESSION };

/*
 * Use XTAL_GUARANTEED so that MPSL ensures the crystal is stable before
 * granting the timeslot.  ESB needs an accurate HF clock for reliable RF.
 */
static mpsl_timeslot_request_t timeslot_request_earliest = {
    .request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
    .params.earliest.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
    .params.earliest.priority = MPSL_TIMESLOT_PRIORITY_NORMAL,
    .params.earliest.length_us = TIMESLOT_LENGTH_US,
    .params.earliest.timeout_us = TIMESLOT_REQUEST_TIMEOUT_US,
};

static mpsl_timeslot_signal_return_param_t signal_callback_return_param;

/* Message queue for requesting MPSL API calls from the non-preemptible thread
 */
K_MSGQ_DEFINE(mpsl_api_msgq, sizeof(enum mpsl_timeslot_call), 10, 4);

static void schedule_request(enum mpsl_timeslot_call call) {
    int err = k_msgq_put(&mpsl_api_msgq, &call, K_NO_WAIT);
    if (err) {
        LOG_ERR("schedule_request failed: %d", err);
        k_oops();
    }
}

static void set_timeslot_active_status(bool active) {
    if (active) {
        if (!m_in_timeslot) {
            m_in_timeslot = true;
            m_callback(APP_TS_STARTED);
        }
    } else {
        if (m_in_timeslot) {
            m_in_timeslot = false;
            m_callback(APP_TS_STOPPED);
        }
    }
}

static mpsl_timeslot_signal_return_param_t *
mpsl_timeslot_callback(mpsl_timeslot_session_id_t session_id,
                       uint32_t signal_type) {
    session_id;
    static bool timeslot_extension_failed;
    mpsl_timeslot_signal_return_param_t *p_ret_val = NULL;

    switch (signal_type) {

    case MPSL_TIMESLOT_SIGNAL_START:
        /* Reset RADIO to clear any leftover BLE configuration. */
        NVIC_ClearPendingIRQ(RADIO_IRQn);
        NRF_RADIO->POWER = RADIO_POWER_POWER_Disabled << RADIO_POWER_POWER_Pos;
        NRF_RADIO->POWER = RADIO_POWER_POWER_Enabled << RADIO_POWER_POWER_Pos;
        NVIC_ClearPendingIRQ(RADIO_IRQn);

        /* TIMER0 is reset at slot start and clocked at 1 MHz. */
        nrf_timer_bit_width_set(MPSL_TIMER0, NRF_TIMER_BIT_WIDTH_32);

        /* CC0: trigger extension request before slot end. */
        nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
                         TIMER_EXPIRY_US_EARLY);
        nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);

        /* CC1: fallback – request a fresh slot if extension failed. */
        nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL1, TIMER_EXPIRY_REQ);
        nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);

        timeslot_extension_failed = false;

        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;

        set_timeslot_active_status(true);
        break;

    case MPSL_TIMESLOT_SIGNAL_TIMER0:
        if (nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0)) {
            /* CC0: attempt to extend the timeslot. */
            nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
            nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);

            signal_callback_return_param.callback_action =
                MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
            signal_callback_return_param.params.extend.length_us =
                TIMESLOT_LENGTH_US;
        } else if (nrf_timer_event_check(MPSL_TIMER0,
                                         NRF_TIMER_EVENT_COMPARE1)) {
            /* CC1: extension window is over. */
            nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
            nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);

            if (timeslot_extension_failed) {
                /* Request a new earliest timeslot. */
                signal_callback_return_param.callback_action =
                    MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST;
                signal_callback_return_param.params.request.p_next =
                    &timeslot_request_earliest;
            } else {
                signal_callback_return_param.callback_action =
                    MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
            }
        }
        p_ret_val = &signal_callback_return_param;
        break;

    case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
        /* Advance both compare values by one slot length. */
        {
            uint32_t cc0 = nrf_timer_cc_get(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0);
            uint32_t cc1 = nrf_timer_cc_get(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL1);

            nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
                             cc0 + TIMESLOT_LENGTH_US);
            nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);

            nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL1,
                             cc1 + TIMESLOT_LENGTH_US);
            nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
        }

        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;
        break;

    case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
        /*
         * Extension was denied.  Stop ESB and let CC1 fire to request a
         * new timeslot.  We remain inside the current (un-extended) slot
         * until CC1 returns SIGNAL_ACTION_REQUEST.
         */
        timeslot_extension_failed = true;

        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;

        set_timeslot_active_status(false);
        break;

    case MPSL_TIMESLOT_SIGNAL_RADIO:
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;

        if (m_in_timeslot) {
            radio_irq_handler();
        } else {
            NVIC_ClearPendingIRQ(RADIO_IRQn);
            NVIC_DisableIRQ(RADIO_IRQn);
        }
        break;

    case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
        LOG_WRN("timeslot overstayed");
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_END;
        p_ret_val = &signal_callback_return_param;
        set_timeslot_active_status(false);
        break;

    case MPSL_TIMESLOT_SIGNAL_CANCELLED:
        LOG_DBG("timeslot cancelled");
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;
        set_timeslot_active_status(false);
        /*
         * Returning SIGNAL_ACTION_REQUEST from CANCELLED causes a hard fault;
         * schedule from thread context instead.
         */
        schedule_request(MAKE_REQUEST);
        break;

    case MPSL_TIMESLOT_SIGNAL_BLOCKED:
        LOG_INF("timeslot blocked");
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;
        set_timeslot_active_status(false);
        schedule_request(MAKE_REQUEST);
        break;

    case MPSL_TIMESLOT_SIGNAL_INVALID_RETURN:
        LOG_WRN("timeslot invalid return");
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_END;
        p_ret_val = &signal_callback_return_param;
        set_timeslot_active_status(false);
        break;

    case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
        LOG_INF("timeslot session idle");
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;
        set_timeslot_active_status(false);
        schedule_request(MAKE_REQUEST);
        break;

    case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
        LOG_INF("timeslot session closed");
        signal_callback_return_param.callback_action =
            MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
        p_ret_val = &signal_callback_return_param;
        set_timeslot_active_status(false);
        break;

    default:
        LOG_ERR("unexpected signal: %u", signal_type);
        k_oops();
        break;
    }

    return p_ret_val;
}

/* Non-preemptible thread that serialises MPSL timeslot API calls */
static void mpsl_nonpreemptible_thread(void) {
    int err;
    enum mpsl_timeslot_call api_call;
    mpsl_timeslot_session_id_t session_id = 0xFFu;

    while (1) {
        if (k_msgq_get(&mpsl_api_msgq, &api_call, K_FOREVER) != 0) {
            continue;
        }

        switch (api_call) {
        case OPEN_SESSION:
            err =
                mpsl_timeslot_session_open(mpsl_timeslot_callback, &session_id);
            if (err) {
                LOG_ERR("session open error: %d", err);
                k_oops();
            }
            break;

        case MAKE_REQUEST:
            err = mpsl_timeslot_request(session_id, &timeslot_request_earliest);
            if (err) {
                LOG_ERR("timeslot request error: %d", err);
                k_oops();
            }
            break;

        case CLOSE_SESSION:
            err = mpsl_timeslot_session_close(session_id);
            if (err) {
                LOG_ERR("session close error: %d", err);
                k_oops();
            }
            break;

        default:
            LOG_ERR("unknown timeslot API call: %d", api_call);
            k_oops();
            break;
        }
    }
}

void timeslot_handler_init(timeslot_callback_t callback) {
    m_callback = callback;
    schedule_request(OPEN_SESSION);
    schedule_request(MAKE_REQUEST);
}

K_THREAD_DEFINE(mpsl_nonpreemptible_thread_id, STACKSIZE,
                mpsl_nonpreemptible_thread, NULL, NULL, NULL,
                K_PRIO_COOP(MPSL_THREAD_PRIO), 0, 0);
