#ifndef LUMISCRIPT_VM_H
#define LUMISCRIPT_VM_H

#include <lumi/vm.h>

int lumiscript_load(const uint8_t *bytecode, size_t bytecode_size);

int lumiscript_run_init(void);

int lumiscript_run_update(const lumi_vm_inputs *inputs);

int lumiscript_run_render(const lumi_vm_inputs *inputs, size_t key_index,
                          lumi_vm_output *output);

int lumiscript_reset_state(void);

#endif // LUMISCRIPT_VM_H
