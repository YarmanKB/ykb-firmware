#include "kb_handler_private.h"

#include <subsys/bt_connect.h>
#include <subsys/usb_connect.h>

void kb_handler_transport_send_kb_report(
    hid_kb_report_t *report, enum kb_handler_transport_priority prio) {
#if CONFIG_BT_CONNECT_KBD && CONFIG_USB_CONNECT_KBD
    bool usb_ready = usb_connect_can_send_kb_report();
    bool bt_ready = bt_connect_can_send_kb_report();
    if (usb_ready && bt_ready) {
        if (prio == KBH_TRANSPORT_PRIO_USB) {
            usb_connect_send_kb_report(report);
            return;
        }
        if (prio == KBH_TRANSPORT_PRIO_BT) {
            bt_connect_send_kb_report(report);
        }
    } else if (usb_ready) {
        usb_connect_send_kb_report(report);
    } else if (bt_ready) {
        bt_connect_send_kb_report(report);
    }
#elif CONFIG_BT_CONNECT_KBD
    if (bt_connect_can_send_kb_report()) {
        bt_connect_send_kb_report(report);
    }
#elif CONFIG_USB_CONNECT_KBD
    if (usb_connect_can_send_kb_report()) {
        usb_connect_send_kb_report(report);
    }
#endif // CONFIG_BT_CONNECT_KBD && CONFIG_USB_CONNECT_KBD
}

void kb_handler_transport_send_mouse_report(
    hid_mouse_report_t *report, enum kb_handler_transport_priority prio) {
#if CONFIG_BT_CONNECT_MOUSE && CONFIG_USB_CONNECT_MOUSE
    bool usb_ready = usb_connect_can_send_mouse_report();
    bool bt_ready = bt_connect_can_send_mouse_report();
    if (usb_ready && bt_ready) {
        if (prio == KBH_TRANSPORT_PRIO_USB) {
            usb_connect_send_mouse_report(report);
            return;
        }
        if (prio == KBH_TRANSPORT_PRIO_BT) {
            bt_connect_send_mouse_report(report);
            return;
        }
    } else if (usb_ready) {
        usb_connect_send_mouse_report(report);
    } else if (bt_ready) {
        bt_connect_send_mouse_report(report);
    }
#elif CONFIG_BT_CONNECT_MOUSE
    if (bt_connect_can_send_mouse_report()) {
        bt_connect_send_mouse_report(report);
    }
#elif CONFIG_USB_CONNECT
    if (usb_connect_can_send_mouse_report()) {
        usb_connect_send_mouse_report(report);
    }
#endif // CONFIG_BT_CONNECT && CONFIG_USB_CONNECT
}
