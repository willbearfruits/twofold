# ui: portable screens / gestures / animation. Rendering into gfx, data from params + ui_sys.h.
idf_component_register(SRCS "ui_core.c" "ui_widgets.c" "ui_home.c" "ui_menu.c" "ui_eq.c"
                            "ui_spectrum.c" "ui_fx.c" "ui_diag.c" "ui_boot.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gfx params dsp)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
