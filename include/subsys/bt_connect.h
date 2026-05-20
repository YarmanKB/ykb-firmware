#ifndef LIB_BT_CONNECT_H
#define LIB_BT_CONNECT_H

#include <subsys/usb_connect.h>
#include <subsys/ykb_battsense.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/iterable_sections.h>

struct bt_connect_cb {
    void (*on_connect)(const bt_addr_le_t *addr);
    void (*on_disconnect)(const bt_addr_le_t *addr);
};

#define BT_CONNECT_CB_DEFINE(name)                                             \
    static STRUCT_SECTION_ITERABLE(bt_connect_cb, __bt_connect_cb__##name)

void bt_connect_send_kb_report(const hid_kb_report_t *report);

void bt_connect_send_mouse_report(const hid_mouse_report_t *report);

bool bt_connect_can_send_kb_report(void);
bool bt_connect_can_send_mouse_report(void);

void bt_connect_set_battery_level(uint8_t percentage);
void bt_connect_set_secondary_battery_state(
    const ykb_battsense_state_t *state);
void bt_connect_clear_secondary_battery_state(void);

#endif // LIB_BT_CONNECT_H
