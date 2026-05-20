zephyr_library()

get_filename_component(YKBFIRMWARE_BASE "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

if(NOT DEFINED SPLITLINK_PAIR_ID OR "${SPLITLINK_PAIR_ID}" STREQUAL "")
    set(SPLITLINK_PAIR_ID "dev-default" CACHE STRING
        "Deterministic splitlink pair identifier used to derive the ESB address")
endif()

if("${SPLITLINK_PAIR_ID}" STREQUAL "dev-default")
    message(WARNING
        "SPLITLINK_PAIR_ID is not set. Using development fallback 'dev-default'. "
        "Left and right halves built this way will all share the same splitlink ESB address."
    )
endif()

set(GENERATED_SPLITLINK_ADDRESS_H
    ${CMAKE_CURRENT_BINARY_DIR}/generated_splitlink_address.h
)

add_custom_command(
    OUTPUT
        ${GENERATED_SPLITLINK_ADDRESS_H}
    COMMAND
        ${PYTHON_EXECUTABLE}
        ${YKBFIRMWARE_BASE}/scripts/gen_splitlink_address.py
        --pair-id ${SPLITLINK_PAIR_ID}
        --out-h ${GENERATED_SPLITLINK_ADDRESS_H}
    DEPENDS
        ${YKBFIRMWARE_BASE}/scripts/gen_splitlink_address.py
    VERBATIM
)

zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})

zephyr_library_sources_ifdef(CONFIG_SPLITLINK_YKB_ESB_PRX prx.c)
zephyr_library_sources_ifdef(CONFIG_SPLITLINK_YKB_ESB_PTX ptx.c)
zephyr_library_sources(${GENERATED_SPLITLINK_ADDRESS_H})
