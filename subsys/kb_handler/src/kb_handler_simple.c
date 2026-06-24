#include "kb_handler_private.h"

#include <zephyr/kernel.h>

BUILD_ASSERT(KEY_COUNT_SLAVE == 0,
             "KB_HANDLER_SIMPLE does not support kb-handler-key-count-slave");

SYS_INIT(kb_handler_core_init, POST_KERNEL, CONFIG_KB_HANDLER_INIT_PRIORITY);
