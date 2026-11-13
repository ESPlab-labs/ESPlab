# ESPlab firmware — the thin ESP32 adapter over the portable core.
#
# Everything that makes ESPlab *ESPlab* lives in core/ (portable C, tested on
# the host). This directory only wires the core to real hardware:
#   - ESP-NOW radio      -> the transport
#   - UART0 serial line  -> the console
#   - flash partition    -> the KV store (M4)
idf_component_register(SRCS "main.c" "esplab_now.c" "console_uart.c"
                       "../../core/src/glados.c" "../../core/src/vm.c" "../../core/src/console.c"
                       INCLUDE_DIRS "." "../../core/inc")
