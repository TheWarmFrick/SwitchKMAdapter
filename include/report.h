#ifndef REPORT_H
#define REPORT_H

#include "SwitchDescriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

void report_init(void);
void set_global_procon_state(const ProconIdxState *rpt);
void get_global_procon_state(ProconIdxState *rpt);

#ifdef __cplusplus
}
#endif

#endif
