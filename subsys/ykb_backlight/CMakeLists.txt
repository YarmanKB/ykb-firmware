zephyr_library()

zephyr_library_sources(
    src/lumiscript_vm.c
    src/ykb_backlight.c
)

get_filename_component(YKBFIRMWARE_BASE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include(${YKBFIRMWARE_BASE}/cmake/ykb_board_assets.cmake)
ykb_resolve_board_asset(BACKLIGHT_LAYOUT_FILE resources/backlight_layout.txt REQUIRED)

# Resource generation:

set(BACKLIGHT_RESOURCE_DIR ${YKBFIRMWARE_BASE}/resources/lumiscript)
get_filename_component(LUMISCRIPT_MODULE_DIR
    "${YKBFIRMWARE_BASE}/../modules/lumiscript"
    ABSOLUTE
)

file(GLOB LUMISCRIPT_FILES
    CONFIGURE_DEPENDS
    "${BACKLIGHT_RESOURCE_DIR}/*.lumi"
)

list(SORT LUMISCRIPT_FILES)

file(GLOB LUMISCRIPT_COMPILER_SOURCES
    CONFIGURE_DEPENDS
    "${LUMISCRIPT_MODULE_DIR}/meson.build"
    "${LUMISCRIPT_MODULE_DIR}/include/lumi/*.h"
    "${LUMISCRIPT_MODULE_DIR}/src/*.c"
    "${LUMISCRIPT_MODULE_DIR}/src/*.h"
)

set(GENERATED_BACKLIGHT_C
    ${CMAKE_CURRENT_BINARY_DIR}/generated_backlight_resources.c
)

set(GENERATED_BACKLIGHT_H
    ${CMAKE_CURRENT_BINARY_DIR}/generated_backlight_resources.h
)

add_custom_command(
    OUTPUT
        ${GENERATED_BACKLIGHT_C}
        ${GENERATED_BACKLIGHT_H}
    COMMAND
        ${PYTHON_EXECUTABLE}
        ${YKBFIRMWARE_BASE}/scripts/gen_ykb_backlight_resources.py
        --out-c ${GENERATED_BACKLIGHT_C}
        --out-h ${GENERATED_BACKLIGHT_H}
        --layout ${BACKLIGHT_LAYOUT_FILE}
        --symbol default_backlight_script_slots
        --lumiscript-dir ${LUMISCRIPT_MODULE_DIR}
        --lumiscript-build-dir ${CMAKE_CURRENT_BINARY_DIR}/lumiscript-host
        --compiled-dir ${CMAKE_CURRENT_BINARY_DIR}/compiled_lumiscript
        ${LUMISCRIPT_FILES}
    DEPENDS
        ${YKBFIRMWARE_BASE}/scripts/gen_ykb_backlight_resources.py
        ${BACKLIGHT_LAYOUT_FILE}
        ${LUMISCRIPT_FILES}
        ${LUMISCRIPT_COMPILER_SOURCES}
    VERBATIM
)

zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})
zephyr_library_sources(${GENERATED_BACKLIGHT_C})
