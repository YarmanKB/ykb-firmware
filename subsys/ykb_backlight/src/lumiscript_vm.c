#include "lumiscript_vm.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

LOG_MODULE_DECLARE(ykb_backlight);

#define CONST_CAP CONFIG_YKB_BL_LUMIVM_CONST_CAPACITY
#define GLOBAL_CAP CONFIG_YKB_BL_LUMIVM_GLOBAL_CAPACITY
#define KEY_VAR_CAP CONFIG_YKB_BL_LUMIVM_KEY_VAR_CAPACITY
#define CODE_CAP CONFIG_YKB_BL_LUMIVM_CODE_CAPACITY
#define STACK_CAP CONFIG_YKB_BL_LUMIVM_STACK_CAPACITY

static lumi_vm_program_storage program_storage;
static lumi_vm_state_storage state_storage;
static lumi_vm_program program;
static lumi_vm_state state;

static float constants[CONST_CAP];
static float initial_globals[GLOBAL_CAP];
static float initial_keys[KEY_VAR_CAP];
static uint8_t init_code[CODE_CAP];
static uint8_t update_code[CODE_CAP];
static uint8_t render_code[CODE_CAP];
static uint32_t globals[GLOBAL_CAP];
static uint32_t keys[KEY_VAR_CAP];
static uint32_t stack[STACK_CAP];

static lumi_vm_requirements req;
static lumi_vm_error error;

static K_MUTEX_DEFINE(lumi_mut);
static bool script_loaded = false;

typedef union {
    uint32_t u32;
    float f32;
} host_cell;

static int size_mul_overflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > ((size_t)-1) / a) {
        return 1;
    }
    *out = a * b;
    return 0;
}

static float vm_rand_impl(void *user_data) {
    uint32_t r = sys_rand32_get();
    r >>= 8;
    return (float)r / 16777216.0f;
}

int lumiscript_load(const uint8_t *bytecode, size_t bytecode_size) {
    int err = 0;
    k_mutex_lock(&lumi_mut, K_FOREVER);

    lumi_vm_set_random(&state, vm_rand_impl, NULL);

    if (!lumi_vm_measure_bytecode(bytecode, bytecode_size, &req, &error)) {
        LOG_ERR("lumi_vm_measure_bytecode: %s (%zu)", error.message,
                error.detail);
        err = -1;
        goto defer;
    }

    size_t total_key_cells;
    if (size_mul_overflow(KEY_VAR_CAP, CONFIG_KB_SETTINGS_KEY_COUNT,
                          &total_key_cells)) {
        LOG_ERR("lumivm: key storage overflow");
        err = -2;
        goto defer;
    }

    program_storage = (lumi_vm_program_storage){
        .constants = constants,
        .constant_capacity = CONST_CAP,
        .initial_globals = initial_globals,
        .global_capacity = GLOBAL_CAP,
        .initial_keys = initial_keys,
        .key_capacity = KEY_VAR_CAP,
        .init_code = init_code,
        .init_capacity = CODE_CAP,
        .update_code = update_code,
        .update_capacity = CODE_CAP,
        .render_code = render_code,
        .render_capacity = CODE_CAP,
    };
    state_storage = (lumi_vm_state_storage){
        .globals = globals,
        .global_capacity = GLOBAL_CAP,
        .keys = keys,
        .key_cell_capacity = total_key_cells,
        .stack = stack,
        .stack_capacity = STACK_CAP,
    };

    if (!lumi_vm_can_load(&req, &program_storage, &state_storage,
                          CONFIG_KB_SETTINGS_KEY_COUNT, &error)) {
        LOG_ERR("lumi_vm_can_load: %s (%zu)", error.message, error.detail);
        err = -3;
        goto defer;
    }
    if (!lumi_vm_load_program(bytecode, bytecode_size, &program_storage,
                              &program, &error)) {
        LOG_ERR("lumi_vm_load_program: %s (%zu)", error.message, error.detail);
        err = -4;
        goto defer;
    }
    if (!lumi_vm_init_state(&program, &state_storage,
                            CONFIG_KB_SETTINGS_KEY_COUNT, &state, &error)) {
        LOG_ERR("lumi_vm_init_state: %s (%zu)", error.message, error.detail);
        err = -5;
        goto defer;
    }
    script_loaded = true;

defer:
    k_mutex_unlock(&lumi_mut);
    return err;
}

int lumiscript_run_init(void) {
    int err = 0;
    k_mutex_lock(&lumi_mut, K_FOREVER);

    if (!script_loaded) {
        LOG_ERR("Script is not loaded");
        err = -1;
        goto defer;
    }

    lumi_vm_output output;
    if (!lumi_vm_run_init(&program, &state, &output, &error)) {
        LOG_ERR("lumivm init: %s (%zu)", error.message, error.detail);
        err = -2;
        goto defer;
    }
    LOG_DBG("lumivm init %zu instr", output.instructions_executed);

defer:
    k_mutex_unlock(&lumi_mut);
    return err;
}

int lumiscript_run_update(const lumi_vm_inputs *inputs) {
    int err = 0;
    k_mutex_lock(&lumi_mut, K_FOREVER);

    if (!script_loaded) {
        LOG_ERR("Script is not loaded");
        err = -1;
        goto defer;
    }

    if (!inputs) {
        err = -EINVAL;
        goto defer;
    }

    lumi_vm_output output;
    if (!lumi_vm_run_update(&program, &state, inputs, &output, &error)) {
        LOG_ERR("lumivm update: %s (%zu)", error.message, error.detail);
        err = -2;
        goto defer;
    }
    LOG_DBG("lumivm update %zu instr", output.instructions_executed);

defer:
    k_mutex_unlock(&lumi_mut);
    return err;
}

int lumiscript_run_render(const lumi_vm_inputs *inputs, size_t key_index,
                          lumi_vm_output *output) {
    int err = 0;
    k_mutex_lock(&lumi_mut, K_FOREVER);

    if (!script_loaded) {
        LOG_ERR("Script is not loaded");
        err = -1;
        goto defer;
    }

    if (!inputs || !output) {
        err = -EINVAL;
        goto defer;
    }

    if (!lumi_vm_run_render(&program, &state, key_index, inputs, output,
                            &error)) {
        LOG_ERR("lumivm render: %s (%zu)", error.message, error.detail);
        err = -2;
        goto defer;
    }
    LOG_DBG("lumivm render %zu instr", output->instructions_executed);

defer:
    k_mutex_unlock(&lumi_mut);
    return err;
}

int lumiscript_reset_state(void) {
    int err = 0;
    k_mutex_lock(&lumi_mut, K_FOREVER);

    if (!script_loaded) {
        LOG_ERR("Script is not loaded");
        err = -1;
        goto defer;
    }

    lumi_vm_reset_state(&program, &state);

defer:
    k_mutex_unlock(&lumi_mut);
    return err;
}
