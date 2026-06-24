#include <subsys/splitlink_sync.h>

#include <zephyr/kernel.h>

SYS_INIT(splitlink_sync_master_attach_kb_handler, POST_KERNEL,
         CONFIG_KB_HANDLER_INIT_PRIORITY);
