#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/types.h>

#include <lib/ykb_esb.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void) {
    int err = ykb_esb_rpc_start();
    if (err) {
        LOG_ERR("ykb_esb_rpc_start failed: %d", err);
    }

    while (true) {
        k_sleep(K_FOREVER);
    }
}
