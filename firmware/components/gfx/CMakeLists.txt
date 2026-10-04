# gfx: portable 1-bit graphics + TWOFOLD pixel fonts. No dependencies.
# gfx_fonts.c is generated from fonts/*.txt by tools/fontconv.py and committed (no Python at build time).
idf_component_register(SRCS "gfx.c" "gfx_fonts.c"
                       INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
