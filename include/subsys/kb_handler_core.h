#ifndef SUBSYS_KB_HANDLER_CORE_H
#define SUBSYS_KB_HANDLER_CORE_H

#include <stdbool.h>
#include <stdint.h>

// API mainly for splitlink sync glue

int kb_handler_core_init(void);

void kb_handler_core_get_values(uint16_t *out_values, uint16_t count);

void kb_handler_core_handle_value(uint16_t key_index, uint16_t value);
void kb_handler_core_handle_slave_values(const uint16_t *values,
                                         uint16_t count);
void kb_handler_core_handle_slave_reset(void);

#endif // SUBSYS_KB_HANDLER_CORE_H
