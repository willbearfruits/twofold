cmake_minimum_required(VERSION 3.16)
project(twofold_sim C)

# Host simulator for the TWOFOLD UI: compiles the real firmware gfx / ui / params / rbj sources.
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/components)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")

add_executable(twofold_sim
  sim_main.c
  ui_sys_sim.c
  ${FW}/gfx/gfx.c
  ${FW}/gfx/gfx_fonts.c
  ${FW}/ui/ui_core.c
  ${FW}/ui/ui_widgets.c
  ${FW}/ui/ui_home.c
  ${FW}/ui/ui_menu.c
  ${FW}/ui/ui_eq.c
  ${FW}/ui/ui_spectrum.c
  ${FW}/ui/ui_fx.c
  ${FW}/ui/ui_diag.c
  ${FW}/ui/ui_boot.c
  ${FW}/params/params.c
  ${FW}/params/factory_presets.c
  ${FW}/dsp/rbj.c
)
target_include_directories(twofold_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW}/gfx/include
  ${FW}/ui/include
  ${FW}/ui
  ${FW}/params/include
  ${FW}/dsp/include
)
target_compile_options(twofold_sim PRIVATE -Wall -Wextra -Werror)
target_link_libraries(twofold_sim m)
