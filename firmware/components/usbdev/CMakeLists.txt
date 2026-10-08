# usbdev: TinyUSB (espressif/tinyusb, used directly) UAC2 + USB-MIDI device. See usbdev.c / usb_descriptors.c.
# The include dir tusb_config/ is also injected into the TinyUSB target from firmware/CMakeLists.txt.
idf_component_register(SRCS "usbdev.c" "usb_descriptors.c" "usb_midi.c"
                       INCLUDE_DIRS "include" "tusb_config"
                       REQUIRES espressif__tinyusb esp_timer
                       PRIV_REQUIRES usb params esp_hw_support log freertos
                       LDFRAGMENTS "linker.lf"
                       # TinyUSB calls our descriptor/class callbacks (and has weak defaults for some):
                       # link every object so the strong definitions always win.
                       WHOLE_ARCHIVE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
