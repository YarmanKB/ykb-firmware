zephyr_library()

zephyr_linker_sources(SECTIONS iterables.ld)

zephyr_library_sources(src/ykb_power.c)
