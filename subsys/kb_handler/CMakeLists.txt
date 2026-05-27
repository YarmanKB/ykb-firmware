zephyr_library()

zephyr_library_sources(src/kb_handler_config.c src/kb_handler_core.c
                       src/kb_handler_transport.c)

zephyr_library_sources_ifdef(CONFIG_KB_HANDLER_SIMPLE src/kb_handler_simple.c)
zephyr_library_sources_ifdef(CONFIG_KB_HANDLER_SPLITLINK_MASTER
                             src/kb_handler_splitlink_master_adapter.c)
zephyr_library_sources_ifdef(CONFIG_KB_HANDLER_SPLITLINK_SLAVE
                             src/kb_handler_splitlink_slave_adapter.c)

zephyr_linker_sources(SECTIONS iterables.ld)
