#ifndef ADAPTER_CONFIG_H_
#define ADAPTER_CONFIG_H_

// Available Input Backend modes
#define BACKEND_WIRELESS_BT 1 // Wireless Bluetooth (CYW43 Bluepad32 on Pico W)
#define BACKEND_USB_HOST    2 // Wired USB Host (Pico-PIO-USB on RP2040 GPIO pins)

// Active Input Backend selection
// Default to BACKEND_USB_HOST (can be set to BACKEND_WIRELESS_BT or overridden via CMake)
#ifndef ADAPTER_INPUT_BACKEND
#define ADAPTER_INPUT_BACKEND BACKEND_USB_HOST
#endif

// USB Host GPIO Pin Configuration (used when ADAPTER_INPUT_BACKEND == BACKEND_USB_HOST)
// Pin definitions default to GP2 for D+ and GP3 for D-
#ifndef PIN_USB_HOST_DP
#define PIN_USB_HOST_DP 2 // GPIO 2 = USB D+ (Physical Pin 4 on Pico)
#endif

#ifndef PIN_USB_HOST_DM
#define PIN_USB_HOST_DM (PIN_USB_HOST_DP + 1) // GPIO 3 = USB D- (Physical Pin 5 on Pico)
#endif

// System clock for Pico-PIO-USB (requires a multiple of 12 MHz; 120 MHz recommended)
#define USB_HOST_SYS_CLOCK_KHZ 120000

#endif // ADAPTER_CONFIG_H_
