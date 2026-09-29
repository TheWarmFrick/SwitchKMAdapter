#ifndef USB_HOST_BACKEND_H_
#define USB_HOST_BACKEND_H_

#ifdef __cplusplus
extern "C" {
#endif

void usb_host_init(void);
void usb_host_core1_task(void);

#ifdef __cplusplus
}
#endif

#endif // USB_HOST_BACKEND_H_
