#include "usb.h"
#include <tusb.h>
#include <stdint.h>
#include <stdbool.h>
#include <pico/stdlib.h>
#include "procon.h"

void usb_core_task(void) {
    // Only initialize the device controller (rhport 0 -> Nintendo Switch dock).
    // The USB host controller (rhport 1) is initialized exclusively on Core 1
    // after tuh_configure() completes with the correct GPIO pins.
    tud_init(BOARD_TUD_RHPORT);

    while (1) {
        tud_task();
        procon_task();
    }
}
