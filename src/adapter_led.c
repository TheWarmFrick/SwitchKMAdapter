#include "adapter_led.h"
#include "adapter_config.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

static uint8_t mounted_count = 0;
static bool is_switch_streaming = false;
static uint32_t last_activity_ms = 0;

void adapter_led_init(void) {
#if (PIN_DEBUG_LED >= 0)
    gpio_init(PIN_DEBUG_LED);
    gpio_set_dir(PIN_DEBUG_LED, GPIO_OUT);
    gpio_put(PIN_DEBUG_LED, 0);
#endif
}

void adapter_led_notify_activity(void) {
    last_activity_ms = to_ms_since_boot(get_absolute_time());
}

void adapter_led_set_devices_mounted(uint8_t count) {
    mounted_count = count;
}

void adapter_led_set_switch_active(bool active) {
    is_switch_streaming = active;
}

void adapter_led_task(void) {
#if (PIN_DEBUG_LED >= 0)
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (mounted_count > 0) {
        // USB devices (keyboard/mouse) are mounted on the host port
        if (now - last_activity_ms < 25) {
            // Dip low briefly on active HID reports for crisp input flicker
            gpio_put(PIN_DEBUG_LED, 0);
        } else {
            // Solid ON indicates devices are ready and connected
            gpio_put(PIN_DEBUG_LED, 1);
        }
    } else if (is_switch_streaming) {
        // Switch is active, but no USB keyboard or mouse detected on GP2/GP3: fast alert blink
        bool blink = ((now / 150) % 2) == 0;
        gpio_put(PIN_DEBUG_LED, blink ? 1 : 0);
    } else {
        // Standby heartbeat: short pulse once per second
        uint32_t phase = now % 1000;
        gpio_put(PIN_DEBUG_LED, phase < 80 ? 1 : 0);
    }
#endif
}
