if(NOT DEFINED BOARD)
  message(FATAL_ERROR "BOARD is not defined")
endif()

string(REPLACE "/" ";" _board_parts "${BOARD}")
list(GET _board_parts 0 _board_name)

file(GLOB _board_dir_candidates LIST_DIRECTORIES true "${APP_DIR}/../boards/*/${_board_name}")
list(LENGTH _board_dir_candidates _board_dir_candidates_len)

if(NOT _board_dir_candidates_len EQUAL 1)
  message(FATAL_ERROR
    "Unable to uniquely resolve board directory for '${BOARD}'. "
    "Candidates: ${_board_dir_candidates}"
  )
endif()

list(GET _board_dir_candidates 0 _ykb_board_dir)

set(SB_CONF_FILE
    "${_ykb_board_dir}/sysbuild.conf"
    CACHE INTERNAL "Board-specific sysbuild configuration file"
)
set(mcuboot_EXTRA_CONF_FILE
    "${_ykb_board_dir}/sysbuild/mcuboot.conf"
    CACHE INTERNAL "Board-specific MCUboot sysbuild fragment"
)
set(hci_ipc_EXTRA_CONF_FILE
    "${_ykb_board_dir}/sysbuild/hci_ipc.conf"
    CACHE INTERNAL "Board-specific hci_ipc sysbuild fragment"
)

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)
