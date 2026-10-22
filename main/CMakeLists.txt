idf_component_register(
    SRCS "main.cpp" "board.cpp" "audio.cpp" "face.cpp" "behavior.cpp"
         "head_touch.cpp" "servo.cpp" "usb_hid.cpp" "log_overlay.cpp" "aec_hook.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_uart esp_driver_gpio esp_timer nvs_flash esp_psram spi_flash
             m5unified m5gfx esp_codec_dev usb_device_uac espressif__tinyusb)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

if(CONFIG_STACKCHAN_USB_HID)
    # Composite UAC + HID: usb_device_uac is built with USB_DEVICE_UAC_AS_PART=y,
    # so we must provide tusb_config.h + descriptors to TinyUSB ourselves.
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
    target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/usb" "${uac_dir}/tusb_uac")
    target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb/usb_descriptors.c")
endif()
