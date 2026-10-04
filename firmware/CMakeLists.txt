cmake_minimum_required(VERSION 3.16)

# sdkconfig.defaults: base config (firmware lead). sdkconfig.defaults.radio: WiFi/BLE/httpd (net/ble owners).
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.radio")

# Development aid only: link weak stand-ins for components that do not exist yet (dsp, ui, gfx, net, ble).
# A real component in components/ always overrides a stub of the same name, so this is harmless once
# everything exists, but release builds must leave it OFF (the default).
option(TWOFOLD_STUBS "Build with stub components for missing modules (development only)" OFF)
if(TWOFOLD_STUBS)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/stubs")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(twofold)

# -------------------------------------------------------------------------------------------------
# TinyUSB integration (PLAN.md 9). We use the managed component espressif/tinyusb directly, without
# esp_tinyusb (whose generated tusb_config.h has no audio class).
# -------------------------------------------------------------------------------------------------
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(tusb_dir espressif__tinyusb COMPONENT_DIR)

# 1) Our tusb_config.h (components/usbdev/tusb_config/) must be visible to TinyUSB's own sources.
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/components/usbdev/tusb_config")

# 2) DWC2 RX-FIFO sizing patch. The ESP32-S3 OTG core has 256 words (1 KB) of FIFO RAM shared by the
#    RX FIFO and all TX FIFOs. TinyUSB sizes the shared RX FIFO for TWO largest OUT packets (the
#    databook's "recommended" value). With UAC2 2ch x 4-byte subslots the OUT packet is 49 frames x 8 B
#    = 392 B, which makes the RX FIFO 226 words and the TX FIFOs (EP0 16 + audio IN 98 + feedback 1 +
#    MIDI 8) no longer fit (357 > 256 words): enumeration would fail on SET_CONFIGURATION.
#    The databook minimum is ONE largest packet + 1 status word (+13 setup +1 NAK + 2 per OUT EP), which
#    gives 127 words and a total of 250/256 words. The single ISO OUT packet per 1 ms frame is popped by
#    the RX-level interrupt long before the next one arrives, so one packet of buffering is sufficient.
#    We compile a patched copy of dcd_dwc2.c instead of the original; configuration fails loudly if
#    a TinyUSB update changes the patched line (re-check the FIFO budget in usbdev/usb_descriptors.c).
set(dwc2_dir "${tusb_dir}/src/portable/synopsys/dwc2")
set(dwc2_src "${dwc2_dir}/dcd_dwc2.c")
file(READ "${dwc2_src}" dwc2_text)
set(dwc2_old "return (uint16_t)(13 + 1 + 2 * ((largest_ep_size / 4) + 1) + 2 * ep_count);")
set(dwc2_new "return (uint16_t)(13 + 1 + ((largest_ep_size / 4) + 1) + 2 * ep_count); /* TWOFOLD: 1x largest OUT packet, see firmware/CMakeLists.txt */")
string(FIND "${dwc2_text}" "${dwc2_old}" dwc2_pos)
if(dwc2_pos EQUAL -1)
    message(FATAL_ERROR "TWOFOLD: TinyUSB dcd_dwc2.c changed; the RX FIFO sizing patch no longer applies. "
                        "Re-check the DWC2 FIFO budget (components/usbdev/usb_descriptors.c) and update the patch.")
endif()
string(REPLACE "${dwc2_old}" "${dwc2_new}" dwc2_text "${dwc2_text}")
set(dwc2_patched "${CMAKE_BINARY_DIR}/tusb_patched/dcd_dwc2.c")
file(WRITE "${dwc2_patched}.tmp" "${dwc2_text}")
configure_file("${dwc2_patched}.tmp" "${dwc2_patched}" COPYONLY)   # only touches the file when it changed
get_target_property(tusb_srcs ${tusb_lib} SOURCES)
list(FIND tusb_srcs "${dwc2_src}" dwc2_idx)
if(dwc2_idx EQUAL -1)
    message(FATAL_ERROR "TWOFOLD: dcd_dwc2.c not found in the TinyUSB target sources: ${tusb_srcs}")
endif()
list(REMOVE_ITEM tusb_srcs "${dwc2_src}")
list(APPEND tusb_srcs "${dwc2_patched}")
set_property(TARGET ${tusb_lib} PROPERTY SOURCES ${tusb_srcs})
target_include_directories(${tusb_lib} PRIVATE "${dwc2_dir}")   # for "dwc2_common.h" etc.
