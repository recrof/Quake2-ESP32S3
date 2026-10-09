idf_component_register(
    SRCS
        esp_main.c
        esp_video.c
        esp_display.c
        esp_sound.c
        esp_input.c
        esp_touch.c
        esp_hid_parser.c
        esp_ble_hid.c
        esp_board_l2.c
        esp_console.c
        font8x8.c
    INCLUDE_DIRS
        .
    REQUIRES
        quake2
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_usb_serial_jtag
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_sdspi
        sdmmc
        fatfs
        esp_psram
        esp_partition
        esp_timer
        nvs_flash
        bt
        esp_hid
)
# Board selection from the command line: -DBOARD=BOARD_GENERIC
if(DEFINED BOARD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD=${BOARD})
endif()
# The sound driver is only referenced by the engine (linked after main)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u SNDDMA_Init")
