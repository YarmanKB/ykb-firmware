#include "kb_handler_private.h"

#include <subsys/splitlink_sync.h>

#include <zephyr/kernel.h>

KSCAN_CB_DEFINE(kb_handler_splitlink_master_adapter) = {
    .on_event = splitlink_sync_master_on_local_key_event,
    .on_new_value = splitlink_sync_master_on_local_value,
};

static int kb_handler_splitlink_master_adapter_init(void) {
    return splitlink_sync_master_attach_kb_handler();
}

SYS_INIT(kb_handler_splitlink_master_adapter_init, POST_KERNEL,
         CONFIG_KB_HANDLER_INIT_PRIORITY);
