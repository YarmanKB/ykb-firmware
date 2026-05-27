#ifndef KB_HANDLER_PRIVATE_H
#define KB_HANDLER_PRIVATE_H

#include <subsys/kb_handler.h>
#include <subsys/kb_handler_internal_api.h>
#include <subsys/usb_connect.h>
#include <subsys/zephyr_user_helpers.h>

#include <drivers/kscan.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEY_COUNT Z_USER_PROP(kb_handler_key_count)
#define KEY_COUNT_SLAVE Z_USER_PROP_OR(kb_handler_key_count_slave, 0)

size_t kb_handler_kscan_count(void);
const struct device *kb_handler_get_kscan(size_t idx);

void kb_handler_transport_send_kb_report(
    hid_kb_report_t *report, enum kb_handler_transport_priority prio);
void kb_handler_transport_send_mouse_report(
    hid_mouse_report_t *report, enum kb_handler_transport_priority prio);
void kb_handler_core_get_values(uint16_t *values, uint16_t count);

#endif // KB_HANDLER_PRIVATE_H
