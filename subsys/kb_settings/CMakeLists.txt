zephyr_library()

get_filename_component(YKBFIRMWARE_BASE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

include(${YKBFIRMWARE_BASE}/cmake/ykb_board_assets.cmake)
ykb_resolve_board_asset(DEFAULT_SETTINGS_FILE resources/default_settings.txt REQUIRED)

set(GENERATED_DEFAULT_SETTINGS_C
    ${CMAKE_CURRENT_BINARY_DIR}/generated_default_settings.c
)
set(GENERATED_DEFAULT_SETTINGS_H
    ${CMAKE_CURRENT_BINARY_DIR}/generated_default_settings.h
)

add_custom_command(
    OUTPUT
        ${GENERATED_DEFAULT_SETTINGS_C}
        ${GENERATED_DEFAULT_SETTINGS_H}
    COMMAND
        ${PYTHON_EXECUTABLE}
        ${YKBFIRMWARE_BASE}/scripts/gen_default_settings.py
        --layout ${DEFAULT_SETTINGS_FILE}
        --out-c ${GENERATED_DEFAULT_SETTINGS_C}
        --out-h ${GENERATED_DEFAULT_SETTINGS_H}
    DEPENDS
        ${YKBFIRMWARE_BASE}/scripts/gen_default_settings.py
        ${DEFAULT_SETTINGS_FILE}
    VERBATIM
)

zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})
zephyr_library_sources(src/kb_settings.c)
zephyr_library_sources(${GENERATED_DEFAULT_SETTINGS_C})

zephyr_linker_sources(SECTIONS iterables.ld)
