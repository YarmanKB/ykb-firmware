#include <subsys/kb_handler_core.h>
#include <subsys/splitlink_sync.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(kb_handler);

BUILD_ASSERT(KEY_COUNT_SLAVE > 0,
             "KB_HANDLER_SPLITLINK_SLAVE requires kb-handler-key-count-slave");

SYS_INIT(splitlink_sync_slave_attach_kb_handler, POST_KERNEL,
         CONFIG_KB_HANDLER_INIT_PRIORITY);

static void on_local_scan_frame(struct kbh_runtime_state *st) {
    splitlink_sync_slave_update_values(st->local_scan_values,
                                       LOCAL_SCAN_KEY_COUNT);
}

KB_HANDLER_CORE_HOOK_DEFINE(splitlink_master) = {
    .on_local_scan_frame = on_local_scan_frame,
};
