#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "SwitchDescriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

void procon_init(void);
void procon_task(void);
bool procon_is_streaming(void);

#ifdef __cplusplus
}
#endif
