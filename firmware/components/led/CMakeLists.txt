# led: onboard WS2812 status LED (espressif/led_strip, RMT backend).
idf_component_register(SRCS "led.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board params dsp esp_driver_rmt esp_timer log freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
