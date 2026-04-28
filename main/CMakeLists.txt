idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_usb_serial_jtag esp_timer nvs_flash
)

# Forward -DBOARD_NAME=... from the build command line through to the
# preprocessor so board_config.h can pick the right preset. CI sets this
# per matrix entry; local builds without the flag fall back to the default
# defined inside board_config.h (Thing Plus).
if(DEFINED BOARD_NAME)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_NAME=${BOARD_NAME})
endif()
