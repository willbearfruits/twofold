# main: boot sequence, ctrl/sys tasks, UI task and the firmware side of ui_sys.h.
# (stubs/ holds development-only stand-in components, see TWOFOLD_STUBS in the top CMakeLists.txt.)
idf_component_register(SRCS "app_main.c" "ctrl.c" "hostlink.c" "ui_sys_fw.c" "ui_task.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bootdiag board params audio dsp ui gfx audio usbdev display input storage led net ble
                                     esp_app_format esp_timer heap log freertos esp_system
                       # ui_sys_fw.c is called from components linked after main (ui, net): keep all objects.
                       WHOLE_ARCHIVE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
