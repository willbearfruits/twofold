idf_component_register(SRCS "bootdiag.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_system)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
