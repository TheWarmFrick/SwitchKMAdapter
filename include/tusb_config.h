#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "adapter_config.h"

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// ROOT HUB PORTS
//   RHPort0: Native RP2040 USB (Micro-USB / USB-C) -> Nintendo Switch dock
//   RHPort1: Pico-PIO-USB on GPIO pins -> USB-A OTG Host (keyboard / mouse)
//--------------------------------------------------------------------
#define BOARD_TUD_RHPORT 0

#if (ADAPTER_INPUT_BACKEND == BACKEND_USB_HOST)
#define BOARD_TUH_RHPORT 1
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1

// USB Host settings (supports USB Hubs + multiple gaming keyboards and mice)
#define CFG_TUH_ENUMERATION_BUFSIZE 1024
#define CFG_TUH_HUB 4
#define CFG_TUH_DEVICE_MAX 8
#define CFG_TUH_HID 8
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#else // BACKEND_WIRELESS_BT

#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0

#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION (Nintendo Switch Pro Controller Emulation)
//--------------------------------------------------------------------
#define BOARD_DEVICE_RHPORT_NUM 0
#define BOARD_DEVICE_RHPORT_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | BOARD_DEVICE_RHPORT_SPEED)

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

// Class drivers: HID enabled for Pro Controller
#define CFG_TUD_HID 1
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// HID buffer size for Switch Pro Controller reports (0x30 full input report is 64 bytes)
#define CFG_TUD_HID_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */