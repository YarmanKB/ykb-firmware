#include "kb_handler_private.h"

#include <subsys/splitlink_sync.h>

#include <zephyr/kernel.h>

KSCAN_CB_DEFINE(kb_handler_splitlink_master_adapter) = {
    .on_event = kb_handler_core_handle_key_event,
    .on_new_value = kb_handler_core_handle_value,
};

SYS_INIT(splitlink_sync_master_attach_kb_handler, POST_KERNEL,
         CONFIG_KB_HANDLER_INIT_PRIORITY);
