#ifndef USB_HOST_BACKEND_H_
#define USB_HOST_BACKEND_H_

#include <stdint.h>
#include <stdbool.h>
#include "SwitchDescriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

void usb_host_init(void);
void usb_host_core1_task(void);
void usb_host_get_procon_state(ProconState *st);
uint8_t usb_host_get_device_count(void);

#ifdef __cplusplus
}
#endif

#endif // USB_HOST_BACKEND_H_
