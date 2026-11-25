# Self-signed TLS certificate for the HTTPS server.
# Generated at configure time into the build directory (already git-ignored), so the private
# key can never be committed. Note: `idf.py fullclean` / `set-target` removes it and the next
# build issues a new certificate, so the browser warning has to be accepted again.
# Use a new directory so an existing build cannot reuse the old host certificate.
set(CERT_DIR "${CMAKE_BINARY_DIR}/certs/network_remote")
set(CERT_FILE "${CERT_DIR}/servercert.pem")
set(KEY_FILE "${CERT_DIR}/prvtkey.pem")

if(NOT EXISTS "${CERT_FILE}" OR NOT EXISTS "${KEY_FILE}")
    find_program(OPENSSL_EXECUTABLE openssl)
    if(NOT OPENSSL_EXECUTABLE)
        message(FATAL_ERROR "openssl was not found. Install it to generate the self-signed certificate.")
    endif()
    file(MAKE_DIRECTORY "${CERT_DIR}")
    message(STATUS "Generating self-signed certificate in ${CERT_DIR}")
    execute_process(
        COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes
                -keyout "${KEY_FILE}" -out "${CERT_FILE}" -days 730
                -subj "/O=FTDX10 FTX-1 Network Remote/CN=trx-remote.local"
                -addext "subjectAltName=DNS:trx-remote.local,DNS:trx-remote,DNS:ftx-1c.local"
        RESULT_VARIABLE cert_result
        ERROR_VARIABLE cert_error)
    if(NOT cert_result EQUAL 0)
        message(FATAL_ERROR "Certificate generation failed: ${cert_error}")
    endif()
    file(CHMOD "${KEY_FILE}" PERMISSIONS OWNER_READ OWNER_WRITE)
endif()

idf_component_register(SRCS "web_auth.c" "console_menu.c" "app_config.c" "main.c" "usb_audio.c" "wifi_sta.c" "web_server.c" "https_identity.c" "ota.c" "cat.c" "tx.c" "tx_swr.c" "tx_tone.c" "tx_voice.c" "acc_scope.c" "acc_frame.c" "build_info.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp-tls esp_wifi esp_netif esp_event nvs_flash esp_https_server espressif__mdns lwip
                                  app_update esp_app_format mbedtls esp_timer espressif__usb espressif__usb_host_cdc_acm
                                  esp_driver_usb_serial_jtag esp_driver_uart esp_driver_gpio esp_driver_spi esp_ringbuf
                    EMBED_TXTFILES "tx_mic.js" "index.html" "map.html" "${CERT_FILE}" "${KEY_FILE}"
                    )

# build_info.c carries __DATE__ / __TIME__; force it to be recompiled whenever any other source
# or embedded page changes, so the build time shown in the GUI is always current.
# (set_source_files_properties is not scriptable, so skip it during IDF's requirements scan.)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Base the validity dates on UTC at configure time without requiring a device clock.
    string(TIMESTAMP HTTPS_CERT_YEAR "%Y" UTC)
    string(TIMESTAMP HTTPS_CERT_MONTH "%m" UTC)
    string(TIMESTAMP HTTPS_CERT_DAY "%d" UTC)
    math(EXPR HTTPS_CERT_EXPIRY_YEAR "${HTTPS_CERT_YEAR} + 10")
    set(HTTPS_CERT_EXPIRY_DAY "${HTTPS_CERT_DAY}")
    if(HTTPS_CERT_MONTH STREQUAL "02" AND HTTPS_CERT_DAY STREQUAL "29")
        set(HTTPS_CERT_EXPIRY_DAY "28")
    endif()
    set_property(SOURCE https_identity.c APPEND PROPERTY COMPILE_DEFINITIONS
        HTTPS_CERT_NOT_BEFORE="${HTTPS_CERT_YEAR}${HTTPS_CERT_MONTH}${HTTPS_CERT_DAY}000000"
        HTTPS_CERT_NOT_AFTER="${HTTPS_CERT_EXPIRY_YEAR}${HTTPS_CERT_MONTH}${HTTPS_CERT_EXPIRY_DAY}000000")
    # Redirect the managed UAC component's ring calls only. The application
    # provides an explicit PSRAM backing store for the 16 KiB TX ring.
    idf_component_get_property(uac_target espressif__usb_host_uac COMPONENT_LIB)
    target_sources(${uac_target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/uac_ringbuffer.c"
        "${CMAKE_CURRENT_LIST_DIR}/uac_tx_transfer.c")
    target_compile_definitions(${uac_target} PRIVATE
        xRingbufferCreate=ftx_uac_ring_create
        vRingbufferDelete=ftx_uac_ring_delete
        usb_host_transfer_submit=ftx_uac_transfer_submit)

    file(GLOB BUILD_STAMP_DEPS "${CMAKE_CURRENT_LIST_DIR}/*.c" "${CMAKE_CURRENT_LIST_DIR}/*.h"
                               "${CMAKE_CURRENT_LIST_DIR}/*.html" "${CMAKE_CURRENT_LIST_DIR}/*.js")
    list(REMOVE_ITEM BUILD_STAMP_DEPS "${CMAKE_CURRENT_LIST_DIR}/build_info.c")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/build_info.c"
                                PROPERTIES OBJECT_DEPENDS "${BUILD_STAMP_DEPS}")
endif()
