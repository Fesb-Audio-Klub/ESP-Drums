idf_component_register(
    SRCS "main.c" "usb_midi.c" "drum_pads.c" "hihat_pedal.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_touch_sens esp_timer
)
