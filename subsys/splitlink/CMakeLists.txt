zephyr_library()

zephyr_library_sources(src/splitlink_core.c)
zephyr_linker_sources(SECTIONS iterables.ld)

zephyr_library_sources_ifdef(CONFIG_SPLITLINK_SYNC_MASTER
                             src/sync/splitlink_sync_master.c)
zephyr_library_sources_ifdef(CONFIG_SPLITLINK_SYNC_SLAVE
                             src/sync/splitlink_sync_slave.c)
zephyr_library_sources_ifdef(CONFIG_SPLITLINK_SYNC_IMPL_YKB_PROTO
                             src/sync/splitlink_sync_protocol.c)

add_subdirectory_ifdef(CONFIG_SPLITLINK_BT src/splitlink_bt)
add_subdirectory_ifdef(CONFIG_SPLITLINK_YKB_ESB src/splitlink_ykb_esb)
