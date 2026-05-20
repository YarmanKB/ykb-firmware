zephyr_library()

zephyr_library_sources(
    src/lumiscript_vm.c
    src/ykb_backlight.c
)

get_filename_component(YKBFIRMWARE_BASE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include(${YKBFIRMWARE_BASE}/cmake/ykb_board_assets.cmake)
ykb_resolve_board_asset(BACKLIGHT_LAYOUT_FILE resources/backlight_layout.txt REQUIRED)

# Default settings resources generation:

set(BACKLIGHT_RESOURCE_DIR ${YKBFIRMWARE_BASE}/resources/lumiscript)

file(GLOB LUMISCRIPT_BC_FILES
    CONFIGURE_DEPENDS
    "${BACKLIGHT_RESOURCE_DIR}/*.lbc"
)

list(SORT LUMISCRIPT_BC_FILES)

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
        ${LUMISCRIPT_BC_FILES}
    DEPENDS
        ${YKBFIRMWARE_BASE}/scripts/gen_ykb_backlight_resources.py
        ${BACKLIGHT_LAYOUT_FILE}
        ${LUMISCRIPT_BC_FILES}
    VERBATIM
)

zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})
zephyr_library_sources(${GENERATED_BACKLIGHT_C})
