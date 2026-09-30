#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <stdio.h>
#include "adapter_config.h"
#include "adapter_led.h"
#include "usb.h"
#include "procon.h"

#if (ADAPTER_INPUT_BACKEND == BACKEND_USB_HOST)
#include <hardware/clocks.h>
#include "usb_host_backend.h"

int main(void)
{
    // Pico-PIO-USB requires the system clock to be a multiple of 12 MHz (120 MHz standard)
    set_sys_clock_khz(USB_HOST_SYS_CLOCK_KHZ, true);
    stdio_init_all();

    // Hardware status LED initialization
    adapter_led_init();

#if ENABLE_UART_DEBUG
    printf("\n\n==================================================\n");
    printf(" SwitchKMAdapter Firmware Starting (USB Host Mode)\n");
    printf(" - SysClock:  %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    printf(" - Host D+:   GP%d (Physical Pin 4)\n", PIN_USB_HOST_DP);
    printf(" - Host D-:   GP%d (Physical Pin 5)\n", PIN_USB_HOST_DM);
    printf(" - UART TX:   GP0  (Physical Pin 1) @ 115200 baud\n");
    printf(" - Status LED: GP%d\n", PIN_DEBUG_LED);
    printf("==================================================\n\n");
#endif

    // Initialize Pro Controller data structures, unique MAC and SPI calibration ROM
    procon_init();

    // Initialize USB Host structures
    usb_host_init();

    // Core 1 runs the USB Host stack (Pico-PIO-USB) collecting keyboard and mouse reports
    multicore_reset_core1();
    multicore_launch_core1(usb_host_core1_task);
    sleep_ms(10);

    // Core 0 runs the USB Device stack (Pro Controller emulation -> Nintendo Switch dock)
    usb_core_task();

    return 0;
}

#elif (ADAPTER_INPUT_BACKEND == BACKEND_WIRELESS_BT)
#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/async_context.h>
#include <uni.h>
#include "sdkconfig.h"

// Defined in pico_switch_platform.c
struct uni_platform *get_my_platform(void);

void bluepad_core_task(void)
{
    if (cyw43_arch_init()) {
        return;
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    uni_platform_set_custom(get_my_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();
}

int main(void)
{
    stdio_init_all();
    adapter_led_init();

#if ENABLE_UART_DEBUG
    printf("\nSwitchKMAdapter Firmware Starting (Wireless Bluetooth Mode)\n");
#endif

    // Initialize Pro Controller data structures, unique MAC and SPI calibration ROM
    procon_init();

    // Core 1 runs Bluepad32 Bluetooth stack
    multicore_launch_core1(bluepad_core_task);

    // Core 0 runs USB Pro Controller emulation
    usb_core_task();

    return 0;
}

#endif
