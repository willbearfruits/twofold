# input: EC11 (PCNT 4x quadrature) + debounced switch -> ui_event_t queue.
idf_component_register(SRCS "input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES board ui esp_driver_pcnt esp_driver_gpio esp_timer log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
