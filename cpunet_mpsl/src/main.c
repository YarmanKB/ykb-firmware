#include <zephyr/console/console.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/types.h>

#include <esb.h>

#include <lib/ykb_esb.h>

#include <zephyr/drivers/gpio.h>

#include <zephyr/drivers/bluetooth.h>

#include "bt_hci.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

// #define ESB_RPC_START_DELAY_MS 3000

// static void esb_rpc_start_thread(void *p1, void *p2, void *p3) {
//
//     k_sleep(K_MSEC(ESB_RPC_START_DELAY_MS));
// }
//
// /* nrf_rpc_init() opens the IPC instance and registers an endpoint; 1024
// bytes
//  * is not enough and silently crashes the thread before the endpoint is ever
//  * registered, causing a persistent IPC bond timeout on the cpuapp side. */
// K_THREAD_DEFINE(esb_rpc_start_thread_id, 2048, esb_rpc_start_thread, NULL,
// NULL,
//                 NULL, K_PRIO_PREEMPT(8), 0, 0);

int main(void) {
    int err = ykb_esb_rpc_start();
    if (err) {
        LOG_ERR("ykb_esb_rpc_start failed: %d", err);
    }
    /*
     * bt_hci_init() waits up to K_SECONDS(5) for cpuapp to bind the HCI IPC
     * endpoint.  On builds where cpuapp has Bluetooth disabled (e.g. the
     * right/PTX half of a split keyboard), the bind never happens.
     *
     * Retrying bt_hci_init() in a tight loop sends repeated NS_DESTROY /
     * NS_CREATE announcements that corrupt the shared rpmsg endpoint state on
     * cpuapp, ultimately causing an MPU fault there.  Attempt the init exactly
     * once; if cpuapp has BT enabled the bond will complete within 5 s, and if
     * not we continue running ESB-only via nRF RPC.
     */

    err = bt_hci_init();
    if (err) {
        LOG_ERR("bt_hci_init failed: %d (running ESB-only, no BT HCI)", err);
        /*
         * bt_enable_raw() started the SDC/MPSL BT scheduler before the IPC
         * bond timeout.  Without closing the HCI device, MPSL keeps firing
         * periodic BT calibration events (~900 ms cadence) that preempt our
         * ESB timeslot extensions, causing the alive beacon to fail.
         * bt_hci_close() calls sdc_disable(), handing MPSL bandwidth back to
         * the ESB timeslot.  bt_disable() is not available with BT_HCI_RAW.
         */
        bt_hci_close(DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci)));
    } else {
        /* bt_hci_process() contains its own infinite loop and does not
         * return under normal operation. */
        err = bt_hci_process();
        if (err) {
            LOG_ERR("bt_hci_process failed: %d", err);
        }
    }

    /* ESB via nRF RPC remains active.  Idle here if BT HCI is unavailable. */
    while (true) {
        k_sleep(K_FOREVER);
    }
}
