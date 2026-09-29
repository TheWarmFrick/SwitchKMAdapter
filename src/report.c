#include "report.h"
#include <stdbool.h>
#include <string.h>
#include "pico/sync.h"
#include "SwitchDescriptors.h"

// Hardware spinlock critical section between Core 1 (Bluepad32) and Core 0 (TinyUSB Pro Controller)
static critical_section_t state_crit_sec;
static bool crit_sec_initialized = false;
static ProconIdxState shared_state;

void report_init(void) {
    if (!crit_sec_initialized) {
        critical_section_init(&state_crit_sec);
        crit_sec_initialized = true;
    }
}

void set_global_procon_state(const ProconIdxState *src) {
    if (!src) {
        return;
    }
    report_init();
    critical_section_enter_blocking(&state_crit_sec);
    memcpy(&shared_state, src, sizeof(shared_state));
    critical_section_exit(&state_crit_sec);
}

void get_global_procon_state(ProconIdxState *dest) {
    if (!dest) {
        return;
    }
    report_init();
    critical_section_enter_blocking(&state_crit_sec);
    memcpy(dest, &shared_state, sizeof(*dest));
    critical_section_exit(&state_crit_sec);
}
