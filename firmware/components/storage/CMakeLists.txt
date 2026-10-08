# storage: NVS settings / CC map / 8 presets / kv blobs with audio-safe deferred writes (PLAN.md 13).
idf_component_register(SRCS "storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES params dsp usbdev audio nvs_flash esp_timer heap log freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
