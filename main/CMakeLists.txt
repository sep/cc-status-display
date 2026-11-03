# SSH tunnel spike: the ESP32's SSH identity is embedded at build time from
# main/keys/id_ecdsa + main/keys/id_ecdsa.pub (gitignored; see SPIKE-SSH.md).
# When they're absent — clean checkouts, CI — empty placeholders are embedded
# instead so the build still succeeds; the tunnel then logs "no key embedded"
# and stays down. Production will move the identity into NVS.
set(_cc_key_dir "${CMAKE_CURRENT_SOURCE_DIR}/keys")
set(_cc_embed_files "")
foreach(_k id_ecdsa id_ecdsa.pub)
    if(EXISTS "${_cc_key_dir}/${_k}")
        list(APPEND _cc_embed_files "${_cc_key_dir}/${_k}")
    else()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${_k}" "")
        list(APPEND _cc_embed_files "${CMAKE_CURRENT_BINARY_DIR}/${_k}")
    endif()
endforeach()

idf_component_register(
    SRCS "main.cpp" "ssh_tunnel.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_app_format esp_driver_gpio esp_driver_usb_serial_jtag esp_timer nvs_flash
                  esp_wifi esp_netif esp_event lwip mbedtls libssh2
    EMBED_TXTFILES ${_cc_embed_files}
)

# Forward -DBOARD_NAME=... from the build command line through to the
# preprocessor so board_config.h can pick the right preset. CI sets this
# per matrix entry; local builds without the flag fall back to the default
# defined inside board_config.h (Thing Plus).
if(DEFINED BOARD_NAME)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_NAME=${BOARD_NAME})
endif()
