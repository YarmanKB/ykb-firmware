#ifndef SUBSYS_SPLITLINK_SYNC_H
#define SUBSYS_SPLITLINK_SYNC_H

#include <subsys/ykb_battsense.h>

#include <zephyr/sys/iterable_sections.h>

struct splitlink_sync_cb {
    void (*on_connected)(void);
    void (*on_disconnected)(void);
    void (*on_slave_battery_state)(ykb_battsense_state_t *state);
};

// The slave keys and values should be only managed by kb_handler,
// no reason to add iterable sections for those.

int splitlink_sync_master_attach_kb_handler(void);

int splitlink_sync_slave_attach_kb_handler(void);

void splitlink_sync_slave_update_values(const uint16_t *values, uint16_t count);

// Battery notifications, connect/disconnect on the other hand might be used
// in different subsystems
#define SPLITLINK_SYNC_CB(name)                                                \
    STRUCT_SECTION_ITERABLE(splitlink_sync_cb, splitlink_sync_cb_##name)

#endif // SUBSYS_SPLITLINK_SYNC_H
