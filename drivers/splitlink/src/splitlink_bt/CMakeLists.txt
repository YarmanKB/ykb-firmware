zephyr_library_sources_ifdef(CONFIG_SPLITLINK_BT_CENTRAL central.c)
zephyr_library_sources_ifdef(CONFIG_SPLITLINK_BT_PERIPHERAL peripheral.c)

zephyr_linker_sources(SECTIONS ../../iterables.ld)
