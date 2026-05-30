#ifndef __SUBSYS_KB_HANDLER_H_
#define __SUBSYS_KB_HANDLER_H_

#include <subsys/kb_settings.h>
#include <subsys/usb_connect.h>

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>

#define KB_HANDLER_PRESS_PERCENT_MAX 100U

struct kb_handler_cb {
    void (*on_event)(uint16_t index, bool pressed);
    void (*on_new_value)(uint16_t index, uint16_t press_percent);
};

#define KB_HANDLER_CB_DEFINE(name)                                             \
    static STRUCT_SECTION_ITERABLE(kb_handler_cb, __kb_handler_cb__##name)

#endif // __SUBSYS_KB_HANDLER_H_
