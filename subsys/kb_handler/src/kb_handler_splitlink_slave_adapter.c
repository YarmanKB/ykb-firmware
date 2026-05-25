#include "kb_handler_private.h"

#include <subsys/splitlink_sync.h>

#include <zephyr/kernel.h>

BUILD_ASSERT(KEY_COUNT_SLAVE > 0,
             "KB_HANDLER_SPLITLINK_SLAVE requires kb-handler-key-count-slave");

SYS_INIT(splitlink_sync_slave_attach_kb_handler, POST_KERNEL,
         CONFIG_KB_HANDLER_INIT_PRIORITY);
