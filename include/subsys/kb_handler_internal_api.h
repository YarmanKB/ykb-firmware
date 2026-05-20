#ifndef SUBSYS_KB_HANDLER_INTERNAL_API_H
#define SUBSYS_KB_HANDLER_INTERNAL_API_H

#include <subsys/kb_settings.h>

#include <stdbool.h>
#include <stdint.h>

typedef void (*kb_handler_settings_update_cb_t)(const kb_settings_t *settings);

int kb_handler_check_kscans_ready(void);
int kb_handler_validate_kscan_topology(uint16_t expected_key_count);

int kb_handler_core_init(void);
void kb_handler_core_handle_key_event(uint16_t key_index, bool pressed);
void kb_handler_core_handle_value(uint16_t key_index, uint16_t value);
void kb_handler_core_handle_slave_values(const uint16_t *values,
                                         uint16_t count);
void kb_handler_core_handle_slave_reset(void);
int kb_handler_core_get_settings_snapshot(kb_settings_t *settings);
int kb_handler_register_settings_update_cb(kb_handler_settings_update_cb_t cb);

#endif // SUBSYS_KB_HANDLER_INTERNAL_API_H
