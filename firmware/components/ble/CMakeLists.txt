# TWOFOLD ble: BLE-MIDI peripheral (NimBLE). Exists in RAM only while enabled.
idf_component_register(
    SRCS "ble_midi.c"
    INCLUDE_DIRS "include"
    REQUIRES ui                     # ble_midi.h uses ble_state_t from ui_sys.h
    PRIV_REQUIRES bootdiag bt esp_timer params)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
