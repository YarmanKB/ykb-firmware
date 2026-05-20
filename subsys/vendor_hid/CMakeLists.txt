zephyr_library()

if(CONFIG_VENDOR_HID_PROTOCOL_YKB)
    zephyr_library_sources(src/vendor_hid_protocol.c)
endif()
