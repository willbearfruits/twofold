# display: SSD1306 / SH1106 over the IDF 5.5 i2c_master driver, dirty-band flush, brightness curve.
idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board esp_driver_i2c esp_timer log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
