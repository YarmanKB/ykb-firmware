#include <subsys/kb_handler_core.h>
#include <subsys/splitlink_sync.h>
#include <subsys/ykb_metrics.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdatomic.h>

LOG_MODULE_DECLARE(kb_handler);

static struct k_spinlock slave_values_lock;
static uint16_t latest_slave_values[KBH_SLAVE_VALUES_CAPACITY];
static atomic_bool slave_values_msg_pending;
static atomic_bool slave_values_dirty;

static void slave_values_retry_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(slave_values_retry_work,
                               slave_values_retry_work_handler);

enum kbh_thread_msg_type {
    KBH_THREAD_MSG_SLAVE_VALUES = 0U,
    KBH_THREAD_MSG_SLAVE_KEYS_RESET,
};

struct kbh_thread_msg {
    enum kbh_thread_msg_type type;
};

K_MSGQ_DEFINE(kbh_core_msgq, sizeof(struct kbh_thread_msg),
              CONFIG_KB_HANDLER_MSGQ_SIZE, 4);

static void enqueue_slave_values_msg(void) {
    struct kbh_thread_msg data = {
        .type = KBH_THREAD_MSG_SLAVE_VALUES,
    };
    int err = k_msgq_put(&kbh_core_msgq, &data, K_NO_WAIT);

    YKB_METRICS_KB_MSGQ_PUT(YKB_METRICS_KB_MSG_SLAVE_VALUES, err,
                            k_msgq_num_used_get(&kbh_core_msgq));
    if (err) {
        k_work_reschedule(&slave_values_retry_work, K_MSEC(1));
    }
}

static void slave_values_retry_work_handler(struct k_work *work) {
    if (!atomic_load_explicit(&slave_values_msg_pending,
                              memory_order_relaxed)) {
        return;
    }

    enqueue_slave_values_msg();
}

static void handle_slave_values(struct kbh_runtime_state *st,
                                const uint16_t slave_values[KEY_COUNT_SLAVE]) {
    if (KEY_COUNT_SLAVE == 0U) {
        return;
    }

    for (uint16_t i = 0U; i < KEY_COUNT_SLAVE; ++i) {
        process_key_value(st, KEY_COUNT + i, slave_values[i]);
    }

    finish_value_frame(st);
}

static void
copy_latest_slave_values(uint16_t out_values[KBH_SLAVE_VALUES_CAPACITY]) {
    k_spinlock_key_t key = k_spin_lock(&slave_values_lock);
    memcpy(out_values, latest_slave_values, KEY_COUNT_SLAVE * sizeof(uint16_t));
    k_spin_unlock(&slave_values_lock, key);
}

static void handle_thread_msg(struct kbh_runtime_state *st,
                              const struct kbh_thread_msg *msg) {
    switch (msg->type) {
    case KBH_THREAD_MSG_SLAVE_KEYS_RESET:
        if (KEY_COUNT_SLAVE == 0U) {
            break;
        }

        memset(&st->pressed_keys[KEY_COUNT], 0, KEY_COUNT_SLAVE * sizeof(bool));
        memset(&st->current_values[KEY_COUNT], 0,
               KEY_COUNT_SLAVE * sizeof(uint16_t));
        memset(&st->notified_press_percent[KEY_COUNT], 0,
               KEY_COUNT_SLAVE * sizeof(uint16_t));
        memset(&values[KEY_COUNT], 0, KEY_COUNT_SLAVE * sizeof(uint16_t));

        if (st->active_mode == KB_MODE_MOUSESIM) {
            send_mouse_report_if_changed(st);
        }

        if (st->active_mode == KB_MODE_NORMAL ||
            st->active_mode == KB_MODE_MOUSESIM) {
            send_kb_report_if_changed(st);
        } else if (st->active_mode == KB_MODE_RACE) {
            send_race_report_if_changed(st);
        }
        break;
    case KBH_THREAD_MSG_SLAVE_VALUES:
        if (KEY_COUNT_SLAVE > 0U) {
            uint16_t slave_values[KBH_SLAVE_VALUES_CAPACITY];
            bool expected = false;

            atomic_store_explicit(&slave_values_dirty, false,
                                  memory_order_relaxed);
            copy_latest_slave_values(slave_values);
            handle_slave_values(st, slave_values);

            atomic_store_explicit(&slave_values_msg_pending, false,
                                  memory_order_relaxed);
            if (atomic_load_explicit(&slave_values_dirty,
                                     memory_order_relaxed) &&
                atomic_compare_exchange_strong_explicit(
                    &slave_values_msg_pending, &expected, true,
                    memory_order_relaxed, memory_order_relaxed)) {
                enqueue_slave_values_msg();
            }
        }
        break;
    default:
        LOG_WRN("Unknown kb handler thread msg type %u", msg->type);
        break;
    }
}

static void drain_thread_msgs(struct kbh_runtime_state *st) {
    struct kbh_thread_msg msg;

    while (k_msgq_get(&kbh_core_msgq, &msg, K_NO_WAIT) == 0) {
        handle_thread_msg(st, &msg);
    }
}

void kb_handler_core_handle_slave_values(const uint16_t *slave_values,
                                         uint16_t count) {
    bool expected = false;
    k_spinlock_key_t key;

    if (KEY_COUNT_SLAVE == 0U) {
        return;
    }
    if (!slave_values || count != KEY_COUNT_SLAVE) {
        LOG_ERR("Slave values size mismatch");
        return;
    }

    key = k_spin_lock(&slave_values_lock);
    memcpy(latest_slave_values, slave_values, count * sizeof(uint16_t));
    k_spin_unlock(&slave_values_lock, key);
    memcpy(&values[KEY_COUNT], slave_values, count * sizeof(uint16_t));
    atomic_store_explicit(&slave_values_dirty, true, memory_order_relaxed);

    if (!atomic_compare_exchange_strong_explicit(
            &slave_values_msg_pending, &expected, true, memory_order_relaxed,
            memory_order_relaxed)) {
        return;
    }

    enqueue_slave_values_msg();
}

void kb_handler_core_handle_slave_reset(void) {
    struct kbh_thread_msg data = {
        .type = KBH_THREAD_MSG_SLAVE_KEYS_RESET,
    };
    int err = k_msgq_put(&kbh_core_msgq, &data, K_NO_WAIT);

    YKB_METRICS_KB_MSGQ_PUT(YKB_METRICS_KB_MSG_SLAVE_RESET, err,
                            k_msgq_num_used_get(&kbh_core_msgq));
    if (err) {
        LOG_WRN("Slave reset event dropped");
    }
}

static int kb_handler_splitlink_master_init(void) {

    memset(latest_slave_values, 0, sizeof(latest_slave_values));
    atomic_store_explicit(&slave_values_msg_pending, false,
                          memory_order_relaxed);
    atomic_store_explicit(&slave_values_dirty, false, memory_order_relaxed);

    int err;

    err = splitlink_sync_master_attach_kb_handler();
    if (err) {
        LOG_ERR("splitlink_sync_master_attach_kb_handler: %d", err);
    }

    return 0;
}

SYS_INIT(kb_handler_splitlink_master_init, POST_KERNEL,
         CONFIG_KB_HANDLER_INIT_PRIORITY);
