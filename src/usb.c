#include "usb.h"
#include <tusb.h>
#include <stdint.h>
#include <stdbool.h>
#include <pico/stdlib.h>
#include "procon.h"

void usb_core_task(void) {
    tusb_init();
    procon_init();

    while (1) {
        tud_task();
        procon_task();
    }
}
