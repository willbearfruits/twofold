# audio: I2S0 full duplex (both clock modes), audio task on core 1, XSMT, DSP allocators.
idf_component_register(SRCS "audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2s
                       PRIV_REQUIRES board dsp usbdev esp_driver_gpio esp_timer esp_hw_support heap log freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
