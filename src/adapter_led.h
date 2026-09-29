#ifndef ADAPTER_LED_H_
#define ADAPTER_LED_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void adapter_led_init(void);
void adapter_led_notify_activity(void);
void adapter_led_set_devices_mounted(uint8_t count);
void adapter_led_set_switch_active(bool active);
void adapter_led_task(void);

#ifdef __cplusplus
}
#endif

#endif // ADAPTER_LED_H_
