# host/dsp_tests: builds the firmware dsp + params components for Linux and runs the DSP test-suite.
cmake_minimum_required(VERSION 3.16)
project(twofold_dsp_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/components)

file(GLOB DSP_SRC ${FW}/dsp/*.c)
add_library(twofold_params STATIC ${FW}/params/params.c ${FW}/params/factory_presets.c)
target_include_directories(twofold_params PUBLIC ${FW}/params/include)
target_compile_options(twofold_params PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

add_library(twofold_dsp STATIC ${DSP_SRC})
target_include_directories(twofold_dsp PUBLIC ${FW}/dsp/include)
target_link_libraries(twofold_dsp PUBLIC twofold_params m)
target_compile_options(twofold_dsp PRIVATE -Wall -Wextra -Werror -fno-math-errno)

add_executable(dsp_tests dsp_tests.c)
target_link_libraries(dsp_tests twofold_dsp)
target_compile_options(dsp_tests PRIVATE -Wall -Wextra -Werror)
