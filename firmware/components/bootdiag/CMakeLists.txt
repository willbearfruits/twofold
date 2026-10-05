idf_component_register(SRCS "bootdiag.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_system xtensa esp_hw_support heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# capture panics: our __wrap_esp_panic_handler records the backtrace, then calls the real handler
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler" "-u __wrap_esp_panic_handler")
