#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/ipc/ipc_service.h>
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_bt_hci_ipc),                           \
                       zephyr_ipc_openamp_static_vrings)
#include <openamp/rpmsg_virtio.h>
#define IPC_BUF_SIZE                                                           \
    DT_PROP_OR(DT_CHOSEN(zephyr_bt_hci_ipc), zephyr_buffer_size,               \
               RPMSG_BUFFER_SIZE)
#define IPC_MEM_SIZE                                                           \
    (DT_REG_SIZE(DT_PHANDLE(DT_CHOSEN(zephyr_bt_hci_ipc), memory_region)) / 2)
#define MAX_IPC_BLOCKS DIV_ROUND_UP(IPC_MEM_SIZE, IPC_BUF_SIZE)
#elif DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_bt_hci_ipc), zephyr_ipc_icbmsg)
#define MAX_IPC_BLOCKS DT_PROP(DT_CHOSEN(zephyr_bt_hci_ipc), rx_blocks)
#else
#error "IPC backends other than rpmsg or icbmsg are not supported."
#endif

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/net_buf.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_REGISTER(hci_ipc, CONFIG_BT_LOG_LEVEL);

BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CONN) ||
                 IS_ENABLED(CONFIG_BT_HCI_ACL_FLOW_CONTROL),
             "HCI IPC driver can drop ACL data without Controller-to-Host ACL "
             "flow control");

static struct ipc_ept hci_ept;

static K_THREAD_STACK_DEFINE(tx_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
static struct k_thread tx_thread_data;
static K_THREAD_STACK_DEFINE(queue_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
static struct k_thread queue_thread_data;
static K_FIFO_DEFINE(tx_queue);
static K_FIFO_DEFINE(rx_queue);
static K_SEM_DEFINE(ipc_bound_sem, 0, 1);
static bool bt_raw_ready;
static bool worker_threads_started;
static bool endpoint_registered;
static bool ipc_instance_opened;
struct ipc_block_item {
    const void *ptr;
    size_t len;
};
K_MSGQ_DEFINE(ipc_block_queue, sizeof(struct ipc_block_item), MAX_IPC_BLOCKS,
              sizeof(void *));
#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER) ||                                  \
    defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
/* A flag used to store information if the IPC endpoint has already been bound.
 * The end point can't be used before that happens.
 */
static bool ipc_ept_ready;
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER || CONFIG_BT_HCI_VS_FATAL_ERROR */

#define HCI_IPC_CMD 0x01
#define HCI_IPC_ACL 0x02
#define HCI_IPC_SCO 0x03
#define HCI_IPC_EVT 0x04
#define HCI_IPC_ISO 0x05

#define HCI_FATAL_ERR_MSG true
#define HCI_REGULAR_MSG false

static struct net_buf *hci_ipc_cmd_recv(uint8_t *data, size_t remaining) {
    struct bt_hci_cmd_hdr *hdr = (void *)data;
    struct net_buf *buf;

    if (remaining < sizeof(*hdr)) {
        LOG_ERR("Not enough data for command header");
        return NULL;
    }

    buf = bt_buf_get_tx(BT_BUF_CMD, K_NO_WAIT, hdr, sizeof(*hdr));
    if (buf) {
        data += sizeof(*hdr);
        remaining -= sizeof(*hdr);
    } else {
        LOG_ERR("No available command buffers!");
        return NULL;
    }

    if (remaining != hdr->param_len) {
        LOG_ERR("Command payload length is not correct");
        net_buf_unref(buf);
        return NULL;
    }

    if (remaining > net_buf_tailroom(buf)) {
        LOG_ERR("Not enough space in buffer");
        net_buf_unref(buf);
        return NULL;
    }

    LOG_DBG("len %u", hdr->param_len);
    net_buf_add_mem(buf, data, remaining);

    return buf;
}

static struct net_buf *hci_ipc_acl_recv(uint8_t *data, size_t remaining) {
    struct bt_hci_acl_hdr *hdr = (void *)data;
    struct net_buf *buf;

    if (remaining < sizeof(*hdr)) {
        LOG_ERR("Not enough data for ACL header");
        return NULL;
    }

    buf = bt_buf_get_tx(BT_BUF_ACL_OUT, K_NO_WAIT, hdr, sizeof(*hdr));
    if (buf) {
        data += sizeof(*hdr);
        remaining -= sizeof(*hdr);
    } else {
        LOG_ERR("No available ACL buffers!");
        return NULL;
    }

    if (remaining != sys_le16_to_cpu(hdr->len)) {
        LOG_ERR("ACL payload length is not correct");
        net_buf_unref(buf);
        return NULL;
    }

    if (remaining > net_buf_tailroom(buf)) {
        LOG_ERR("Not enough space in buffer");
        net_buf_unref(buf);
        return NULL;
    }

    LOG_DBG("len %u", remaining);
    net_buf_add_mem(buf, data, remaining);

    return buf;
}

static struct net_buf *hci_ipc_iso_recv(uint8_t *data, size_t remaining) {
    struct bt_hci_iso_hdr *hdr = (void *)data;
    struct net_buf *buf;

    if (remaining < sizeof(*hdr)) {
        LOG_ERR("Not enough data for ISO header");
        return NULL;
    }

    buf = bt_buf_get_tx(BT_BUF_ISO_OUT, K_NO_WAIT, hdr, sizeof(*hdr));
    if (buf) {
        data += sizeof(*hdr);
        remaining -= sizeof(*hdr);
    } else {
        LOG_ERR("No available ISO buffers!");
        return NULL;
    }

    if (remaining != bt_iso_hdr_len(sys_le16_to_cpu(hdr->len))) {
        LOG_ERR("ISO payload length is not correct");
        net_buf_unref(buf);
        return NULL;
    }

    if (remaining > net_buf_tailroom(buf)) {
        LOG_ERR("Not enough space in buffer");
        net_buf_unref(buf);
        return NULL;
    }

    LOG_DBG("len %zu", remaining);
    net_buf_add_mem(buf, data, remaining);

    return buf;
}

static void queue_thread(void *p1, void *p2, void *p3) {
    while (1) {
        struct ipc_block_item block;
        const uint8_t *data;
        uint8_t pkt_indicator;
        struct net_buf *buf = NULL;
        size_t remaining;
        int err = k_msgq_get(&ipc_block_queue, &block, K_FOREVER);

        __ASSERT_NO_MSG(err == 0);

        data = block.ptr;
        pkt_indicator = *data++;
        remaining = block.len - sizeof(pkt_indicator);

        switch (pkt_indicator) {
        case HCI_IPC_CMD:
            buf = hci_ipc_cmd_recv((uint8_t *)data, remaining);
            break;

        case HCI_IPC_ACL:
            buf = hci_ipc_acl_recv((uint8_t *)data, remaining);
            break;

        case HCI_IPC_ISO:
            buf = hci_ipc_iso_recv((uint8_t *)data, remaining);
            break;

        default:
            LOG_ERR("Unknown HCI type %u", pkt_indicator);
            break;
        }

        err = ipc_service_release_rx_buffer(&hci_ept, (void *)block.ptr);
        if (err < 0) {
            LOG_ERR("Failed to release rx buffer: %d", err);
        }

        if (buf) {
            k_fifo_put(&tx_queue, buf);
            LOG_HEXDUMP_DBG(buf->data, buf->len, "Final net buffer:");
        }
    }
}

static void tx_thread(void *p1, void *p2, void *p3) {
    while (1) {
        struct net_buf *buf;
        int err;

        /* Wait until a buffer is available */
        buf = k_fifo_get(&tx_queue, K_FOREVER);
        /* Pass buffer to the stack */
        err = bt_send(buf);
        if (err) {
            LOG_ERR("Unable to send (err %d)", err);
            net_buf_unref(buf);
        }

        /* Give other threads a chance to run if tx_queue keeps getting
         * new data all the time.
         */
        k_yield();
    }
}

static void hci_ipc_send(struct net_buf *buf, bool is_fatal_err) {
    uint8_t retries = 0;
    int ret;

    LOG_DBG("buf %p type %u len %u", buf, buf->data[0], buf->len);

    LOG_HEXDUMP_DBG(buf->data, buf->len, "Final HCI buffer:");

    do {
        ret = ipc_service_send(&hci_ept, buf->data, buf->len);
        if (ret < 0) {
            retries++;
            if (retries > 10) {
                /* Default backend (rpmsg_virtio) has a timeout of 150ms. */
                LOG_WRN("IPC send has been blocked for 1.5 seconds.");
                retries = 0;
            }

            /* The function can be called by the application main thread,
             * bt_ctlr_assert_handle and k_sys_fatal_error_handler. In case of a
             * call by Bluetooth Controller assert handler or system fatal error
             * handler the call can be from ISR context, hence there is no
             * thread to yield. Besides that both handlers implement a policy to
             * provide error information and stop the system in an infinite
             * loop. The goal is to prevent any other damage to the system if
             * one of such exeptional situations occur, hence call to k_yield is
             * against it.
             */
            if (is_fatal_err) {
                LOG_ERR("ipc_service_send error: %d", ret);
            } else {
                /* In the POSIX ARCH, code takes zero simulated time to execute,
                 * so busy wait loops become infinite loops, unless we
                 * force the loop to take a bit of time.
                 *
                 * This delay allows the IPC consumer to execute, thus making
                 * it possible to send more data over IPC afterwards.
                 */
                Z_SPIN_DELAY(500);
                k_yield();
            }
        }
    } while (ret < 0);

    LOG_INF("ipc_service_send sent %d/%u bytes", ret, buf->len);
    __ASSERT_NO_MSG(ret == buf->len);

    net_buf_unref(buf);
}

#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER)
void bt_ctlr_assert_handle(char *file, uint32_t line) {
    /* Disable interrupts, this is unrecoverable */
    irq_lock();

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
    /* Generate an error event only when IPC service endpoint is already bound.
     */
    if (ipc_ept_ready) {
        /* Prepare vendor specific HCI debug event */
        struct net_buf *buf;

        buf = hci_vs_err_assert(file, line);
        if (buf != NULL) {
            /* Send the event over ipc */
            hci_ipc_send(buf, HCI_FATAL_ERR_MSG);
        } else {
            LOG_ERR("Can't create Fatal Error HCI event: %s at %d", __FILE__,
                    __LINE__);
        }
    } else {
        LOG_ERR("IPC endpoint is not ready yet: %s at %d", __FILE__, __LINE__);
    }

    LOG_ERR("Halting system");

#else /* !CONFIG_BT_HCI_VS_FATAL_ERROR */
    LOG_ERR("Controller assert in: %s at %d", file, line);

#endif /* !CONFIG_BT_HCI_VS_FATAL_ERROR */

    /* Flush the logs before locking the CPU */
    LOG_PANIC();

    while (true) {
        k_cpu_idle();
    };

    CODE_UNREACHABLE;
}
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER */

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
void k_sys_fatal_error_handler(unsigned int reason,
                               const struct arch_esf *esf) {
    /* Disable interrupts, this is unrecoverable */
    irq_lock();

    /* Generate an error event only when there is a stack frame and IPC service
     * endpoint is already bound.
     */
    if (esf != NULL && ipc_ept_ready) {
        /* Prepare vendor specific HCI debug event */
        struct net_buf *buf;

        buf = hci_vs_err_stack_frame(reason, esf);
        if (buf != NULL) {
            hci_ipc_send(buf, HCI_FATAL_ERR_MSG);
        } else {
            LOG_ERR("Can't create Fatal Error HCI event.\n");
        }
    }

    LOG_ERR("Halting system");

    /* Flush the logs before locking the CPU */
    LOG_PANIC();

    while (true) {
        k_cpu_idle();
    };

    CODE_UNREACHABLE;
}
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

static void hci_ept_bound(void *priv) {
    k_sem_give(&ipc_bound_sem);
#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER) ||                                  \
    defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
    ipc_ept_ready = true;
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER || CONFIG_BT_HCI_VS_FATAL_ERROR */
}

static void hci_ept_recv(const void *data, size_t len, void *priv) {
    struct ipc_block_item block = {
        .ptr = data,
        .len = len,
    };
    int err;

    LOG_INF("Received message of %u bytes.", len);
    LOG_HEXDUMP_DBG(data, len, "IPC data:");

    err = ipc_service_hold_rx_buffer(&hci_ept, (void *)data);
    if (err) {
        LOG_ERR("Failed to hold rx buffer: %d", err);
        return;
    }

    err = k_msgq_put(&ipc_block_queue, &block, K_NO_WAIT);
    if (err) {
        LOG_ERR("Failed to queue IPC rx buffer: %d", err);
        int release_err = ipc_service_release_rx_buffer(&hci_ept, (void *)data);
        if (release_err < 0) {
            LOG_ERR("Failed to release rx buffer after queue error: %d",
                    release_err);
        }
    }
}

static struct ipc_ept_cfg hci_ept_cfg = {
    .name = "nrf_bt_hci",
    .cb =
        {
            .bound = hci_ept_bound,
            .received = hci_ept_recv,
        },
};

static void start_threads_once(void) {
    if (worker_threads_started) {
        return;
    }

    k_thread_create(&tx_thread_data, tx_thread_stack,
                    K_THREAD_STACK_SIZEOF(tx_thread_stack), tx_thread, NULL,
                    NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(&tx_thread_data, "HCI ipc TX");
    k_thread_create(&queue_thread_data, queue_thread_stack,
                    K_THREAD_STACK_SIZEOF(queue_thread_stack), queue_thread,
                    NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(&queue_thread_data, "HCI ipc RX");
    worker_threads_started = true;
}

static void reset_ipc_startup_state(const struct device *hci_ipc_instance) {
    if (endpoint_registered) {
        int err = ipc_service_deregister_endpoint(&hci_ept);
        if (err < 0) {
            LOG_WRN("Failed to deregister HCI endpoint: %d", err);
        }
        endpoint_registered = false;
    }

    if (ipc_instance_opened) {
        int err = ipc_service_close_instance(hci_ipc_instance);
        if (err < 0) {
            LOG_WRN("Failed to close HCI IPC instance: %d", err);
        }
        ipc_instance_opened = false;
    }
}

int bt_hci_init(void) {
    int err;
    const struct device *hci_ipc_instance =
        DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci_ipc));

    LOG_DBG("Start");

    /* Enable the raw interface, this will in turn open the HCI driver */
    if (!bt_raw_ready) {
        err = bt_enable_raw(&rx_queue);
        if (err) {
            LOG_ERR("bt_enable_raw failed: %d", err);
            return err;
        }
        bt_raw_ready = true;
    }

    start_threads_once();

    /* Initialize IPC service instance and register endpoint. */
    err = ipc_service_open_instance(hci_ipc_instance);
    if (err < 0 && err != -EALREADY) {
        LOG_ERR("IPC service instance initialization failed: %d\n", err);
        return err;
    }
    ipc_instance_opened = true;

    err =
        ipc_service_register_endpoint(hci_ipc_instance, &hci_ept, &hci_ept_cfg);
    if (err) {
        LOG_ERR("Registering endpoint failed with %d", err);
        reset_ipc_startup_state(hci_ipc_instance);
        return err;
    }
    endpoint_registered = true;

    err = k_sem_take(&ipc_bound_sem, K_SECONDS(5));
    if (err) {
        LOG_ERR("Endpoint binding failed with %d", err);
        reset_ipc_startup_state(hci_ipc_instance);
        return err;
    }

    return 0;
}

int bt_hci_process(void) {
    while (1) {
        struct net_buf *buf = k_fifo_get(&rx_queue, K_FOREVER);
        hci_ipc_send(buf, HCI_REGULAR_MSG);
    }

    return 0;
}
