#ifndef SUBSYS_SPLITLINK_SYNC_H
#define SUBSYS_SPLITLINK_SYNC_H

#include <zephyr/sys/iterable_sections.h>

struct splitlink_sync_battery_notify {
    void (*cb)();
};

// The slave keys and values should be only managed by kb_handler,
// no reason to add iterable sections for those.

int splitlink_sync_master_attach_kb_handler(void);

int splitlink_sync_slave_attach_kb_handler(void);

// Battery notifications on the other hand might be used in different subsystems
#if CONFIG_SPLITLINK_SYNC_MASTER
#define SPLITLINK_SYNC_BATTERY_STATE_CB(name, cb)                              \
    STRUCT_SECTION_ITERABLE(name, )
#else
#define SPLITLINK_SYNC_BATTERY_STATE_CB(name, cb)
#endif // CONFIG_SPLITLINK_SYNC_MASTER

#endif // SUBSYS_SPLITLINK_SYNC_H
