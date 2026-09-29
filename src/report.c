#include "report.h"

#include <stdbool.h>
#include <string.h>
#include <pico/multicore.h>
#include <pico/async_context.h>
#include <pico/cyw43_arch.h>

#include "usb.h"
#include "SwitchDescriptors.h"

// Thread-safe shared state between Core 1 (Bluepad32) and Core 0 (TinyUSB Pro Controller)
static ProconIdxState shared_state;
static uint32_t unused;

void set_global_procon_state(const ProconIdxState *src) {
    if (!src) {
        return;
    }

    async_context_t *context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(context);
    memcpy(&shared_state, src, sizeof(shared_state));
    async_context_release_lock(context);
    multicore_fifo_push_timeout_us(0, 1);
}

void get_global_procon_state(ProconIdxState *dest) {
    if (!dest) {
        return;
    }

    multicore_fifo_pop_timeout_us(1, &unused);
    async_context_t *context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(context);
    memcpy(dest, &shared_state, sizeof(*dest));
    async_context_release_lock(context);
}