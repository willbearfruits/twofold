# dsp: the TWOFOLD audio engine (portable C11, float32). Built for the ESP32-S3 here and for Linux by
# host/dsp_tests (which compiles the same sources without ESP_PLATFORM).
idf_component_register(SRCS "dsp_engine.c" "rbj.c" "smooth.c" "biquad.c" "eq.c" "halfband.c" "sat.c"
                            "chorus.c" "delay.c" "reverb.c" "comp.c" "limiter.c" "dither.c"
                       INCLUDE_DIRS "include"
                       REQUIRES params esp_hw_support
                       LDFRAGMENTS "linker.lf")
# -O2 everywhere (the project may build with -Os); no -ffast-math: bit-transparent paths and host/target
# agreement rely on IEEE semantics (x * 1.0f, exact int24 <-> float), and fast-math would also let GCC
# reassociate the halfband / limiter sums. Denormals are handled explicitly (flush + DSP_TINY offsets).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra -Werror -fno-math-errno)
