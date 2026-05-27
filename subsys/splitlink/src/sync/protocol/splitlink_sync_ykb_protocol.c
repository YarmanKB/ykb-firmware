#include "../splitlink_sync_private.h"
#include <subsys/splitlink.h>
#include <subsys/splitlink_sync.h>

#define YKB_PROTOCOL_MAX_PACKET_SIZE SPLITLINK_MAX_PACKET_LENGTH
#include <lib/ykb_protocol.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <stdatomic.h>

LOG_MODULE_REGISTER(splitlink_sync, CONFIG_SPLITLINK_LOG_LEVEL);

enum rx_slot_state {
    RX_SLOT_EMPTY,
    RX_SLOT_RECEIVING,
    RX_SLOT_READY,
    RX_SLOT_PROCESSING,
};

enum tx_slot_state {
    TX_SLOT_EMPTY,
    TX_SLOT_TRANSCEIVING,
};

struct rx_slot {
    struct k_work work;
    enum rx_slot_state state;
    uint8_t *data;
    uint16_t max_data_length;
#if CONFIG_SPLITLINK_OUT_OF_ORDER_TRACK
    uint8_t bitmap[CONFIG_SPLITLINK_BITMAP_LENGTH];
#endif // CONFIG_SPLITLINK_OUT_OF_ORDER_TRACK
    ykb_protocol_rx_state_t rx;
    uint8_t id;
};

struct tx_slot {
    struct k_work_delayable work;
    enum tx_slot_state state;
    uint8_t *data;
    uint16_t max_data_length;
    ykb_protocol_tx_state_t tx;
    ykb_protocol_packet_t pending_packet;
    bool pending_packet_ready;
    uint8_t id;
};

#define ATOMIC_STORE(var, val)                                                 \
    atomic_store_explicit(var, val, memory_order_relaxed)
#define ATOMIC_LOAD(var) atomic_load_explicit(var, memory_order_relaxed)
static atomic_bool connected;

#define VALUES_SLOT_ID 1U
#define SETTINGS_SLOT_ID 2U
#define BENCHMARK_SLOT_ID 3U
#define SCRIPT_MANIFEST_SLOT_ID 4U
#define SCRIPT_REQUEST_SLOT_ID 5U
#define SCRIPT_SLOT_SLOT_ID 6U
#define BATTERY_SLOT_ID 7U

#if CONFIG_SPLITLINK_BENCHMARK
struct splitlink_benchmark_payload {
    uint32_t seq;
    uint32_t sent_cycles;
    uint8_t padding[CONFIG_SPLITLINK_BENCHMARK_PAYLOAD_SIZE];
};

BUILD_ASSERT(sizeof(struct splitlink_benchmark_payload) <=
                 YKB_PROTOCOL_MAX_PAYLOAD_SIZE,
             "Splitlink benchmark payload must fit in one packet");
#endif // CONFIG_SPLITLINK_BENCHMARK

#if CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
static void benchmark_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(benchmark_work, benchmark_work_handler);
static atomic_bool benchmark_inflight;
static uint32_t benchmark_seq;
static uint32_t benchmark_min_us;
static uint32_t benchmark_max_us;
static uint32_t benchmark_samples;
static uint32_t benchmark_timeouts;
static uint64_t benchmark_total_us;
#endif // CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER

#define TX_SLOT(NAME, DATA_SIZE, ID)                                           \
    static uint8_t NAME##_tx_slot_data[DATA_SIZE] = {0};                       \
    static struct tx_slot NAME##_tx_slot = {                                   \
        .data = NAME##_tx_slot_data,                                           \
        .max_data_length = sizeof(NAME##_tx_slot_data),                        \
        .state = TX_SLOT_EMPTY,                                                \
        .id = ID,                                                              \
    }

#define RX_SLOT(NAME, DATA_SIZE, ID)                                           \
    static uint8_t NAME##_rx_slot_data[DATA_SIZE] = {0};                       \
    static struct rx_slot NAME##_rx_slot = {                                   \
        .data = NAME##_rx_slot_data,                                           \
        .max_data_length = sizeof(NAME##_rx_slot_data),                        \
        .state = RX_SLOT_EMPTY,                                                \
        .id = ID,                                                              \
    }

#if CONFIG_SPLITLINK_SYNC_SLAVE
TX_SLOT(values, sizeof(uint16_t) * CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE,
        VALUES_SLOT_ID);
TX_SLOT(battery, sizeof(splitlink_battery_state_t), BATTERY_SLOT_ID);
RX_SLOT(settings, sizeof(kb_settings_t), SETTINGS_SLOT_ID);
RX_SLOT(scripts_manifest, sizeof(splitlink_script_manifest_t),
        SCRIPT_MANIFEST_SLOT_ID);
TX_SLOT(scripts_request, sizeof(splitlink_script_request_t),
        SCRIPT_REQUEST_SLOT_ID);
RX_SLOT(script_slot, sizeof(splitlink_script_slot_packet_t),
        SCRIPT_SLOT_SLOT_ID);
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

#if CONFIG_SPLITLINK_SYNC_SLAVE
static uint8_t pending_values_tx_data[sizeof(uint16_t) *
                                      CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE];
static uint16_t pending_values_tx_len;
static bool pending_values_tx_ready;
static splitlink_battery_state_t pending_battery_tx_state;
static bool pending_battery_tx_ready;
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

#if CONFIG_SPLITLINK_SYNC_MASTER
RX_SLOT(values, sizeof(uint16_t) * CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE,
        VALUES_SLOT_ID);
RX_SLOT(battery, sizeof(splitlink_battery_state_t), BATTERY_SLOT_ID);
TX_SLOT(settings, sizeof(kb_settings_t), SETTINGS_SLOT_ID);
TX_SLOT(scripts_manifest, sizeof(splitlink_script_manifest_t),
        SCRIPT_MANIFEST_SLOT_ID);
RX_SLOT(scripts_request, sizeof(splitlink_script_request_t),
        SCRIPT_REQUEST_SLOT_ID);
TX_SLOT(script_slot, sizeof(splitlink_script_slot_packet_t),
        SCRIPT_SLOT_SLOT_ID);
#endif // CONFIG_SPLITLINK_SYNC_MASTER

static inline void rx_init(struct rx_slot *slot) {
#if CONFIG_SPLITLINK_OUT_OF_ORDER_TRACK
    ykb_protocol_rx_init(&slot->rx, slot->data, slot->max_data_length, true,
                         slot->bitmap, sizeof(slot->bitmap));
#else
    ykb_protocol_rx_init(&slot->rx, slot->data, slot->max_data_length, false,
                         NULL, 0);
#endif // CONFIG_SPLITLINK_OUT_OF_ORDER_TRACK
}

#if CONFIG_SPLITLINK_SYNC_MASTER
static K_MUTEX_DEFINE(script_sync_mut);
static bool script_slot_sync_pending[CONFIG_YKB_BL_SCRIPT_SLOT_COUNT];

static void script_slot_sync_kick_locked(void);

static void script_slot_sync_mark_pending_locked(uint16_t slot) {
    if (slot >= ARRAY_SIZE(script_slot_sync_pending)) {
        LOG_ERR("Script slot sync out of range: %u", slot);
        return;
    }

    script_slot_sync_pending[slot] = true;
}

static bool script_slot_sync_take_next_locked(uint16_t *slot) {
    for (uint16_t idx = 0; idx < ARRAY_SIZE(script_slot_sync_pending); ++idx) {
        if (!script_slot_sync_pending[idx]) {
            continue;
        }

        script_slot_sync_pending[idx] = false;
        *slot = idx;
        return true;
    }

    return false;
}
#endif // CONFIG_SPLITLINK_SYNC_MASTER

static inline bool splitlink_connected(void) { return ATOMIC_LOAD(&connected); }

static bool tx_slot_begin_transfer(struct tx_slot *slot, const void *data,
                                   uint16_t data_len) {
    if (slot->state == TX_SLOT_TRANSCEIVING) {
        return false;
    }

    if (data_len > slot->max_data_length) {
        LOG_ERR("TX slot %d overflow (got %d, max %d)", slot->id, data_len,
                slot->max_data_length);
        return false;
    }

    ykb_protocol_tx_init(&slot->tx, slot->data, data_len, slot->id,
                         YKB_PROTOCOL_TYPE_DATA);
    memcpy(slot->data, data, data_len);
    slot->pending_packet_ready = false;
    slot->state = TX_SLOT_TRANSCEIVING;
    k_work_reschedule(&slot->work, K_NO_WAIT);
    return true;
}

static bool tx_slot_begin_transfer_or_log_busy(struct tx_slot *slot,
                                               const void *data,
                                               uint16_t data_len) {
    if (slot->state == TX_SLOT_TRANSCEIVING) {
        LOG_ERR("Slot %d already in progress, skipping", slot->id);
        return false;
    }

    return tx_slot_begin_transfer(slot, data, data_len);
}

#if CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
static inline uint32_t benchmark_cycles_to_us(uint32_t cycles) {
    return (uint32_t)(((uint64_t)cycles * USEC_PER_SEC) /
                      sys_clock_hw_cycles_per_sec());
}

static void benchmark_schedule_next(k_timeout_t delay) {
    if (!ATOMIC_LOAD(&connected)) {
        return;
    }

    k_work_reschedule(&benchmark_work, delay);
}

static void
benchmark_log_sample(const struct splitlink_benchmark_payload *sample) {
    uint32_t rtt_us =
        benchmark_cycles_to_us(k_cycle_get_32() - sample->sent_cycles);

    if (benchmark_samples == 0 || rtt_us < benchmark_min_us) {
        benchmark_min_us = rtt_us;
    }
    if (rtt_us > benchmark_max_us) {
        benchmark_max_us = rtt_us;
    }

    benchmark_samples++;
    benchmark_total_us += rtt_us;

    LOG_INF("Splitlink RTT: seq=%u rtt=%u us avg=%u us min=%u us max=%u us "
            "timeouts=%u",
            sample->seq, rtt_us,
            (uint32_t)(benchmark_total_us / benchmark_samples),
            benchmark_min_us, benchmark_max_us, benchmark_timeouts);
}

static void benchmark_work_handler(struct k_work *work) {
    if (!ATOMIC_LOAD(&connected)) {
        return;
    }

    if (ATOMIC_LOAD(&benchmark_inflight)) {
        benchmark_timeouts++;
        LOG_WRN("Splitlink RTT timeout: seq=%u total_timeouts=%u",
                benchmark_seq, benchmark_timeouts);
        ATOMIC_STORE(&benchmark_inflight, false);
    }

    struct splitlink_benchmark_payload payload = {
        .seq = ++benchmark_seq,
        .sent_cycles = k_cycle_get_32(),
    };
    ykb_protocol_packet_t packet = {0};
    ykb_protocol_tx_state_t tx = {0};

    ykb_protocol_tx_init(&tx, (const uint8_t *)&payload, sizeof(payload),
                         BENCHMARK_SLOT_ID, YKB_PROTOCOL_TYPE_DATA);
    if (!ykb_protocol_tx_build_packet(&tx, &packet)) {
        LOG_ERR("Splitlink RTT packet build failed");
        benchmark_schedule_next(K_MSEC(CONFIG_SPLITLINK_BENCHMARK_INTERVAL_MS));
        return;
    }

    ATOMIC_STORE(&benchmark_inflight, true);

    int err = splitlink_send((uint8_t *)&packet,
                             YKB_PROTOCOL_HEADER_SIZE + sizeof(payload));
    if (err) {
        ATOMIC_STORE(&benchmark_inflight, false);
        LOG_ERR("Splitlink RTT send failed: %d", err);
    }
}
#endif // CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER

void rx_slot_work_handler(struct k_work *work) {
    struct rx_slot *slot = CONTAINER_OF(work, struct rx_slot, work);
    slot->state = RX_SLOT_PROCESSING;

#if CONFIG_SPLITLINK_SYNC_MASTER
    if (slot->id == VALUES_SLOT_ID) {
        uint16_t values[CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE];
        memcpy(values, slot->data, slot->rx.total_len);
        uint16_t total_len = slot->rx.total_len;
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        splitlink_sync_values_received(values, total_len / sizeof(uint16_t));
        return;
    }
    if (slot->id == BATTERY_SLOT_ID) {
        const splitlink_battery_state_t *state =
            (const splitlink_battery_state_t *)slot->data;
        splitlink_sync_battery_state_received(state);
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        return;
    }
    if (slot->id == SCRIPT_REQUEST_SLOT_ID) {
        const splitlink_script_request_t *request =
            (const splitlink_script_request_t *)slot->data;
        splitlink_sync_scripts_request_received(request);
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        return;
    }
#endif // CONFIG_SPLITLINK_SYNC_MASTER
#if CONFIG_SPLITLINK_SYNC_SLAVE
    if (slot->id == SETTINGS_SLOT_ID) {
        const kb_settings_t *settings = (const kb_settings_t *)slot->data;
        splitlink_sync_settings_received(settings);
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        return;
    }
    if (slot->id == SCRIPT_MANIFEST_SLOT_ID) {
        const splitlink_script_manifest_t *manifest =
            (const splitlink_script_manifest_t *)slot->data;
        splitlink_sync_scripts_manifest_received(manifest);
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        return;
    }
    if (slot->id == SCRIPT_SLOT_SLOT_ID) {
        const splitlink_script_slot_packet_t *slot_packet =
            (const splitlink_script_slot_packet_t *)slot->data;
        splitlink_sync_script_slot_received(slot_packet);
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        return;
    }
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

    // Just in case
    ykb_protocol_rx_reset(&slot->rx);
    slot->state = RX_SLOT_EMPTY;
}

void tx_slot_work_handler(struct k_work *work) {
    struct tx_slot *slot =
        CONTAINER_OF(k_work_delayable_from_work(work), struct tx_slot, work);
    uint16_t packet_len;
    int err;

    if (slot->state != TX_SLOT_TRANSCEIVING) {
        return;
    }

    if (!slot->pending_packet_ready) {
        if (!ykb_protocol_tx_has_more(&slot->tx)) {
            slot->state = TX_SLOT_EMPTY;
            goto out;
        }

        if (!ykb_protocol_tx_build_packet(&slot->tx, &slot->pending_packet)) {
            LOG_ERR("ykb_protocol_tx_build_packet failed");
            slot->state = TX_SLOT_EMPTY;
            goto out;
        }

        slot->pending_packet_ready = true;
    }

    packet_len = YKB_PROTOCOL_HEADER_SIZE +
                 ykb_protocol_payload_len_for_index(
                     slot->pending_packet.header.total_len,
                     slot->pending_packet.header.packet_idx,
                     slot->pending_packet.header.packet_count);

    err = splitlink_send((uint8_t *)&slot->pending_packet, packet_len);
    if (err) {
        if (err == -ENOMEM || err == -EAGAIN) {
            k_work_reschedule(&slot->work, K_MSEC(5));
            return;
        }

        LOG_ERR("splitlink_send: %d", err);
        slot->state = TX_SLOT_EMPTY;
        slot->pending_packet_ready = false;
        goto out;
    }

    slot->pending_packet_ready = false;

    if (ykb_protocol_tx_has_more(&slot->tx)) {
        k_work_reschedule(&slot->work, K_NO_WAIT);
        return;
    }

    slot->state = TX_SLOT_EMPTY;

out:
#if CONFIG_SPLITLINK_SYNC_SLAVE
    if (slot->id == VALUES_SLOT_ID && pending_values_tx_ready &&
        slot->state == TX_SLOT_EMPTY) {
        slot->tx.total_len = pending_values_tx_len;
        ykb_protocol_tx_init(&slot->tx, slot->data, pending_values_tx_len,
                             slot->id, YKB_PROTOCOL_TYPE_DATA);
        memcpy(slot->data, pending_values_tx_data, pending_values_tx_len);
        slot->pending_packet_ready = false;
        slot->state = TX_SLOT_TRANSCEIVING;
        pending_values_tx_ready = false;
        k_work_reschedule(&slot->work, K_NO_WAIT);
        return;
    }
    if (slot->id == BATTERY_SLOT_ID && pending_battery_tx_ready &&
        slot->state == TX_SLOT_EMPTY) {
        tx_slot_begin_transfer(slot, &pending_battery_tx_state,
                               sizeof(pending_battery_tx_state));
        pending_battery_tx_ready = false;
        return;
    }
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

#if CONFIG_SPLITLINK_SYNC_MASTER
    if (slot->id == SCRIPT_SLOT_SLOT_ID) {
        k_mutex_lock(&script_sync_mut, K_FOREVER);
        script_slot_sync_kick_locked();
        k_mutex_unlock(&script_sync_mut);
    }
#endif // CONFIG_SPLITLINK_SYNC_MASTER
}

void splitlink_sync_protocol_on_receive(uint8_t *data, size_t data_len) {
    if (!data || data_len == 0) {
        return;
    }

    if (data_len < YKB_PROTOCOL_HEADER_SIZE ||
        data_len > sizeof(ykb_protocol_packet_t)) {
        LOG_ERR("invalid packet length %d", data_len);
        return;
    }

    ykb_protocol_packet_t packet = {0};
    memcpy(&packet, data, data_len);
    uint8_t id = packet.header.transfer_id;

#if CONFIG_SPLITLINK_BENCHMARK
    if (id == BENCHMARK_SLOT_ID) {
        if (packet.header.packet_count != 1 || packet.header.packet_idx != 0 ||
            packet.header.total_len !=
                sizeof(struct splitlink_benchmark_payload)) {
            LOG_ERR("Invalid splitlink RTT packet");
            return;
        }

#if CONFIG_SPLITLINK_SYNC_SLAVE
        int err = splitlink_send(data, data_len);
        if (err) {
            LOG_ERR("Splitlink RTT echo failed: %d", err);
        }
        return;
#elif CONFIG_SPLITLINK_SYNC_MASTER
        struct splitlink_benchmark_payload sample = {0};
        memcpy(&sample, packet.payload, sizeof(sample));
        ATOMIC_STORE(&benchmark_inflight, false);
        benchmark_log_sample(&sample);
        benchmark_schedule_next(K_MSEC(CONFIG_SPLITLINK_BENCHMARK_INTERVAL_MS));
        return;
#endif // CONFIG_SPLITLINK_SYNC_SLAVE / CONFIG_SPLITLINK_SYNC_MASTER
    }
#endif // CONFIG_SPLITLINK_BENCHMARK

    struct rx_slot *slot;

    switch (id) {
    case VALUES_SLOT_ID:
#if CONFIG_SPLITLINK_SYNC_MASTER
        slot = &values_rx_slot;
        break;
#else
        LOG_ERR("Values RX is not supported on splitlink slave");
        return;
#endif // CONFIG_SPLITLINK_SYNC_MASTER
    case SETTINGS_SLOT_ID:
#if CONFIG_SPLITLINK_SYNC_SLAVE
        slot = &settings_rx_slot;
        break;
#else
        LOG_ERR("Settings RX is not supported on splitlink master");
        return;
#endif // CONFIG_SPLITLINK_SYNC_SLAVE
    case BATTERY_SLOT_ID:
#if CONFIG_SPLITLINK_SYNC_MASTER
        slot = &battery_rx_slot;
        break;
#else
        LOG_ERR("Battery RX is not supported on splitlink slave");
        return;
#endif // CONFIG_SPLITLINK_SYNC_MASTER
    case SCRIPT_MANIFEST_SLOT_ID:
#if CONFIG_SPLITLINK_SYNC_SLAVE
        slot = &scripts_manifest_rx_slot;
        break;
#else
        LOG_ERR("Script manifest RX is not supported on splitlink master");
        return;
#endif // CONFIG_SPLITLINK_SYNC_SLAVE
    case SCRIPT_REQUEST_SLOT_ID:
#if CONFIG_SPLITLINK_SYNC_MASTER
        slot = &scripts_request_rx_slot;
        break;
#else
        LOG_ERR("Script request RX is not supported on splitlink slave");
        return;
#endif // CONFIG_SPLITLINK_SYNC_MASTER
    case SCRIPT_SLOT_SLOT_ID:
#if CONFIG_SPLITLINK_SYNC_SLAVE
        slot = &script_slot_rx_slot;
        break;
#else
        LOG_ERR("Script slot RX is not supported on splitlink master");
        return;
#endif // CONFIG_SPLITLINK_SYNC_SLAVE
    default:
        LOG_ERR("Packet for unknown slot id %d", id);
        return;
    }

    ykb_protocol_rx_result_t res;

    switch (slot->state) {
    case RX_SLOT_EMPTY:
        rx_init(slot);
        slot->state = RX_SLOT_RECEIVING;
        res = ykb_protocol_rx_push_packet(&slot->rx, &packet);
        break;
    case RX_SLOT_RECEIVING:
        res = ykb_protocol_rx_push_packet(&slot->rx, &packet);
        break;
    default:
        LOG_ERR("Slot %d is in progress, skipping packet", id);
        return;
    }

    if (res < 0) {
        LOG_ERR("packet transfer: %d", res);
        ykb_protocol_rx_reset(&slot->rx);
        slot->state = RX_SLOT_EMPTY;
        return;
    }

    if (res == YKB_PROTOCOL_RX_RESULT_COMPLETE) {
        slot->state = RX_SLOT_READY;
        k_work_submit(&slot->work);
    }
}

#if CONFIG_SPLITLINK_SYNC_SLAVE
void splitlink_sync_send_values(uint16_t *values, uint16_t count) {
    if (!values || count == 0) {
        LOG_ERR("splitlink_sync_send_values: values null or count 0");
        return;
    }
    if (!splitlink_connected()) {
        return;
    }
    struct tx_slot *slot = &values_tx_slot;
    uint16_t size_bytes = count * sizeof(uint16_t);

    if (slot->state == TX_SLOT_TRANSCEIVING) {
        if (size_bytes > sizeof(pending_values_tx_data)) {
            LOG_ERR("Pending TX slot %d overflow (got %d, max %d)", slot->id,
                    size_bytes, (int)sizeof(pending_values_tx_data));
            return;
        }

        memcpy(pending_values_tx_data, values, size_bytes);
        pending_values_tx_len = size_bytes;
        pending_values_tx_ready = true;
        return;
    }

    tx_slot_begin_transfer(slot, values, size_bytes);
}
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

#if CONFIG_SPLITLINK_SYNC_SLAVE
void splitlink_sync_send_battery_state(const splitlink_battery_state_t *state) {
    struct tx_slot *slot = &battery_tx_slot;

    if (!state || !splitlink_connected()) {
        return;
    }

    if (slot->state == TX_SLOT_TRANSCEIVING) {
        pending_battery_tx_state = *state;
        pending_battery_tx_ready = true;
        return;
    }

    tx_slot_begin_transfer(slot, state, sizeof(*state));
}
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

#if CONFIG_SPLITLINK_SYNC_MASTER
static void script_slot_sync_kick_locked(void) {
    splitlink_script_slot_packet_t slot_packet = {0};
    ykb_backlight_script_slot_t script_payload = {0};
    struct tx_slot *slot = &script_slot_tx_slot;
    uint16_t script_slot;
    int err;

    if (!splitlink_connected() || slot->state == TX_SLOT_TRANSCEIVING) {
        return;
    }

    if (!script_slot_sync_take_next_locked(&script_slot)) {
        return;
    }

    slot_packet.slot = script_slot;
    err = ykb_backlight_get_script_slot(script_slot, &script_payload);
    if (err) {
        LOG_ERR("ykb_backlight_get_script_slot(%u): %d", script_slot, err);
        return;
    }
    memcpy(&slot_packet.payload, &script_payload, sizeof(script_payload));

    tx_slot_begin_transfer(slot, &slot_packet, sizeof(slot_packet));
}
#endif // CONFIG_SPLITLINK_SYNC_MASTER

#if CONFIG_SPLITLINK_SYNC_MASTER
void splitlink_sync_send_settings(const kb_settings_t *settings) {
    if (!settings) {
        LOG_ERR("splitlink_sync_send_settings: settings null");
        return;
    }
    if (!splitlink_connected()) {
        return;
    }
    struct tx_slot *slot = &settings_tx_slot;
    tx_slot_begin_transfer_or_log_busy(slot, settings, sizeof(*settings));
}
#endif // CONFIG_SPLITLINK_SYNC_MASTER

#if CONFIG_SPLITLINK_SYNC_MASTER
void splitlink_sync_send_scripts_manifest(
    const splitlink_script_manifest_t *manifest) {
    struct tx_slot *slot = &scripts_manifest_tx_slot;

    if (!manifest) {
        LOG_ERR("splitlink_sync_send_scripts_manifest: manifest null");
        return;
    }

    if (!splitlink_connected()) {
        return;
    }

    tx_slot_begin_transfer_or_log_busy(slot, manifest, sizeof(*manifest));
}

void splitlink_sync_send_script_slot(
    const splitlink_script_slot_packet_t *slot_packet) {
    struct tx_slot *slot = &script_slot_tx_slot;

    if (!slot_packet) {
        LOG_ERR("splitlink_sync_send_script_slot: slot_packet null");
        return;
    }

    if (!splitlink_connected()) {
        return;
    }

    tx_slot_begin_transfer_or_log_busy(slot, slot_packet, sizeof(*slot_packet));
}

void splitlink_sync_queue_script_slot_sync(uint16_t slot) {
    if (slot >= CONFIG_YKB_BL_SCRIPT_SLOT_COUNT) {
        LOG_ERR("splitlink_sync_queue_script_slot_sync: slot %u out of range",
                slot);
        return;
    }

    k_mutex_lock(&script_sync_mut, K_FOREVER);
    script_slot_sync_mark_pending_locked(slot);
    script_slot_sync_kick_locked();
    k_mutex_unlock(&script_sync_mut);
}
#endif // CONFIG_SPLITLINK_SYNC_MASTER

#if CONFIG_SPLITLINK_SYNC_SLAVE
void splitlink_sync_request_scripts(const splitlink_script_request_t *request) {
    struct tx_slot *slot = &scripts_request_tx_slot;

    if (!request) {
        LOG_ERR("splitlink_sync_request_scripts: request null");
        return;
    }

    if (!splitlink_connected()) {
        return;
    }

    tx_slot_begin_transfer_or_log_busy(slot, request, sizeof(*request));
}
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

int splitlink_sync_init(void) {

    ATOMIC_STORE(&connected, false);

#if CONFIG_SPLITLINK_SYNC_MASTER
    k_work_init(&values_rx_slot.work, rx_slot_work_handler);
    k_work_init(&battery_rx_slot.work, rx_slot_work_handler);
    k_work_init_delayable(&settings_tx_slot.work, tx_slot_work_handler);
    k_work_init_delayable(&scripts_manifest_tx_slot.work, tx_slot_work_handler);
    k_work_init(&scripts_request_rx_slot.work, rx_slot_work_handler);
    k_work_init_delayable(&script_slot_tx_slot.work, tx_slot_work_handler);
#endif // CONFIG_SPLITLINK_SYNC_MASTER

#if CONFIG_SPLITLINK_SYNC_SLAVE
    k_work_init(&settings_rx_slot.work, rx_slot_work_handler);
    k_work_init_delayable(&values_tx_slot.work, tx_slot_work_handler);
    k_work_init_delayable(&battery_tx_slot.work, tx_slot_work_handler);
    k_work_init(&scripts_manifest_rx_slot.work, rx_slot_work_handler);
    k_work_init_delayable(&scripts_request_tx_slot.work, tx_slot_work_handler);
    k_work_init(&script_slot_rx_slot.work, rx_slot_work_handler);
    pending_values_tx_len = 0;
    pending_values_tx_ready = false;
    pending_battery_tx_ready = false;
#endif // CONFIG_SPLITLINK_SYNC_SLAVE

#if CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
    benchmark_seq = 0;
    benchmark_min_us = 0;
    benchmark_max_us = 0;
    benchmark_samples = 0;
    benchmark_timeouts = 0;
    benchmark_total_us = 0;
    ATOMIC_STORE(&benchmark_inflight, false);
#endif // CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER

#if CONFIG_SPLITLINK_SYNC_MASTER
    memset(script_slot_sync_pending, 0, sizeof(script_slot_sync_pending));
#endif // CONFIG_SPLITLINK_SYNC_MASTER

    return 0;
}

void splitlink_sync_protocol_on_connect(void) {
    ATOMIC_STORE(&connected, true);
#if CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
    benchmark_schedule_next(K_MSEC(250));
#endif // CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
    splitlink_sync_on_connect();
}

void splitlink_sync_protocol_on_disconnect(void) {
    ATOMIC_STORE(&connected, false);
#if CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
    k_work_cancel_delayable(&benchmark_work);
    ATOMIC_STORE(&benchmark_inflight, false);
#endif // CONFIG_SPLITLINK_BENCHMARK && CONFIG_SPLITLINK_SYNC_MASTER
#if CONFIG_SPLITLINK_SYNC_MASTER
    k_mutex_lock(&script_sync_mut, K_FOREVER);
    memset(script_slot_sync_pending, 0, sizeof(script_slot_sync_pending));
    k_mutex_unlock(&script_sync_mut);
#endif // CONFIG_SPLITLINK_SYNC_MASTER
    splitlink_sync_on_disconnect();
}
