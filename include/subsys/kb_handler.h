#ifndef __SUBSYS_KB_HANDLER_H_
#define __SUBSYS_KB_HANDLER_H_

#include <subsys/kb_settings.h>
#include <subsys/usb_connect.h>

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>

void kb_handler_get_values(uint16_t *values, uint16_t count);

int kb_handler_get_default_thresholds(uint16_t *buffer);
int kb_handler_get_default_minimums(uint16_t *buffer);
int kb_handler_get_default_maximums(uint16_t *buffer);

int kb_handler_get_default_keymap_layer1(uint8_t *buffer);
int kb_handler_get_default_keymap_layer2(uint8_t *buffer);
int kb_handler_get_default_keymap_layer3(uint8_t *buffer);

int kb_handler_get_default_mouseemu(kb_mouseemu_settings_t *buffer);
size_t kb_handler_get_default_fn_shortcuts(const kb_fn_shortcut_t **shortcuts);

#endif // __SUBSYS_KB_HANDLER_H_
