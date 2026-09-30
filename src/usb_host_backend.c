// USB Host Input Backend using Pico-PIO-USB on Core 1.
// Supports USB Keyboards and Mice (direct or via USB Hub) on configurable GPIO pins (default: GP2 D+, GP3 D-).

#include "adapter_config.h"

#if (ADAPTER_INPUT_BACKEND == BACKEND_USB_HOST)

#include "usb_host_backend.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "pico/stdlib.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "pio_usb.h"
#include "tusb.h"

#include "report.h"
#include "SwitchDescriptors.h"
#include "KeyboardKeys.h"
#include "adapter_led.h"

// 12-bit analog stick sensitivity & timing constants
#define MOUSE_STICK_SENSITIVITY 45
#define MOUSE_IDLE_TIMEOUT_MS   35

// IMU Gyro sensitivity constants (360 deg turn mouse counts)
#define GYRO_COUNTS_PER_360     8000.0f
#define PITCH_COUNTS_PER_360    8000.0f

//--------------------------------------------------------------------
// Mouse report layout extracted from the HID report descriptor
//--------------------------------------------------------------------
typedef struct {
    bool valid;
    bool is_mouse;
    bool has_report_id;
    uint8_t xy_report_id;
    uint8_t btn_report_id;
    uint16_t x_off, y_off, btn_off;
    uint8_t x_size, y_size;
    uint8_t btn_count;
} mouse_layout_t;

typedef struct {
    bool used;
    uint8_t dev_addr;
    uint8_t instance;
    uint8_t itf_protocol;
    bool use_layout;
    mouse_layout_t layout;
    hid_keyboard_report_t kb;
    uint8_t mouse_buttons;
} slot_t;

static slot_t slots[CFG_TUH_HID];
static critical_section_t hid_lock;
static int32_t acc_dx = 0;
static int32_t acc_dy = 0;

static uint32_t last_mouse_move_time_ms = 0;
static uint16_t current_rx = STICK_CENTER;
static uint16_t current_ry = STICK_CENTER;
static uint8_t active_device_count = 0;

// IMU pending angular values
static float pend_yaw = 0.0f;
static float pend_pitch = 0.0f;

void usb_host_init(void) {
    critical_section_init(&hid_lock);
    memset(slots, 0, sizeof(slots));
    active_device_count = 0;
    acc_dx = 0;
    acc_dy = 0;
    pend_yaw = 0.0f;
    pend_pitch = 0.0f;
}

uint8_t usb_host_get_device_count(void) {
    return active_device_count;
}

static slot_t *find_slot(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < CFG_TUH_HID; i++) {
        if (slots[i].used && slots[i].dev_addr == dev_addr && slots[i].instance == instance) {
            return &slots[i];
        }
    }
    return NULL;
}

static void update_device_count(void) {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_HID; i++) {
        if (slots[i].used) count++;
    }
    active_device_count = count;
    adapter_led_set_devices_mounted(count);
}

//--------------------------------------------------------------------
// Minimal HID report descriptor parser for mice (handles gaming mice)
//--------------------------------------------------------------------
#define MAX_REPORT_IDS 8

static uint16_t *cursor_for(uint8_t id, uint8_t *ids, uint16_t *curs, int *n) {
    for (int i = 0; i < *n; i++) {
        if (ids[i] == id) return &curs[i];
    }
    if (*n >= MAX_REPORT_IDS) return NULL;
    ids[*n] = id;
    curs[*n] = 0;
    return &curs[(*n)++];
}

static bool parse_mouse_desc(uint8_t const *d, uint16_t len, mouse_layout_t *out) {
    uint8_t ids[MAX_REPORT_IDS];
    uint16_t curs[MAX_REPORT_IDS];
    int ncurs = 0;

    uint8_t cur_id = 0;
    bool any_id = false;
    uint16_t usage_page = 0;
    uint16_t usages[8];
    int nusages = 0;
    uint32_t rsize = 0, rcount = 0;
    bool seen_collection = false;
    uint8_t xy_id = 0, btn_id = 0;

    memset(out, 0, sizeof(*out));
    if (!d || len == 0) return false;

    uint16_t pos = 0;
    while (pos < len) {
        uint8_t prefix = d[pos++];
        if (prefix == 0xFE) {
            if (pos >= len) break;
            uint8_t l = d[pos++];
            pos += (uint16_t)(l + 2);
            continue;
        }

        uint8_t bsize = prefix & 0x03;
        if (bsize == 3) bsize = 4;
        uint8_t tag  = (uint8_t)(prefix >> 4);
        uint8_t type = (uint8_t)((prefix >> 2) & 0x03);

        uint32_t data = 0;
        for (uint8_t i = 0; i < bsize && pos < len; i++) {
            data |= ((uint32_t)d[pos++]) << (i * 8);
        }

        if (type == 1) { // Global
            switch (tag) {
                case 0x00: usage_page = (uint16_t)data; break;
                case 0x07: rsize = data; break;
                case 0x08: cur_id = (uint8_t)data; any_id = true; break;
                case 0x09: rcount = data; break;
                default: break;
            }
        } else if (type == 2) { // Local
            if (tag == 0x00) {
                if (nusages < 8) usages[nusages++] = (uint16_t)data;
            }
        } else if (type == 0) { // Main
            if (tag == 0x0A) { // Collection
                if (!seen_collection && usage_page == 0x01) {
                    for (int i = 0; i < nusages; i++) {
                        if (usages[i] == 0x02) out->is_mouse = true;
                    }
                }
                seen_collection = true;
                nusages = 0;
            } else if (tag == 0x0C) { // End Collection
                nusages = 0;
            } else if (tag == 0x08 || tag == 0x09) { // Input / Output
                if (tag == 0x08) {
                    uint16_t *c = cursor_for(cur_id, ids, curs, &ncurs);
                    uint16_t base = c ? *c : 0;
                    uint32_t total_bits = rsize * rcount;

                    if (usage_page == 0x01) {
                        for (int i = 0; i < nusages && (uint32_t)i < rcount; i++) {
                            uint16_t off = (uint16_t)(base + i * rsize);
                            if (usages[i] == 0x30 && !out->x_size) {
                                out->x_off = off;
                                out->x_size = (uint8_t)rsize;
                                xy_id = cur_id;
                            } else if (usages[i] == 0x31 && !out->y_size) {
                                out->y_off = off;
                                out->y_size = (uint8_t)rsize;
                            }
                        }
                    } else if (usage_page == 0x09) {
                        if (!out->btn_count) {
                            out->btn_off = base;
                            out->btn_count = (uint8_t)total_bits;
                            btn_id = cur_id;
                        }
                    }
                    if (c) *c = (uint16_t)(base + total_bits);
                }
                nusages = 0;
            }
        }
    }

    if (out->is_mouse && out->x_size && out->y_size) {
        out->valid = true;
        out->has_report_id = any_id;
        out->xy_report_id = xy_id;
        out->btn_report_id = btn_id;
        return true;
    }
    return false;
}

static uint32_t extract_bits(uint8_t const *p, uint16_t off, uint8_t bits) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < bits; i++) {
        uint16_t b = (uint16_t)(off + i);
        if (p[b >> 3] & (1u << (b & 7))) v |= 1u << i;
    }
    return v;
}

static int32_t extract_signed(uint8_t const *p, uint16_t off, uint8_t bits) {
    uint32_t v = extract_bits(p, off, bits);
    if (bits < 32 && (v & (1u << (bits - 1)))) v |= ~((1u << bits) - 1u);
    return (int32_t)v;
}

static void handle_layout_mouse(slot_t *s, uint8_t const *report, uint16_t len) {
    mouse_layout_t const *L = &s->layout;
    uint8_t const *p = report;
    uint16_t plen = len;
    uint8_t rid = 0;

    if (L->has_report_id) {
        if (plen < 2) return;
        rid = p[0];
        p++;
        plen--;
    }
    uint32_t plen_bits = (uint32_t)plen * 8;

    critical_section_enter_blocking(&hid_lock);
    if (!L->has_report_id || rid == L->xy_report_id) {
        uint32_t need_x = (uint32_t)L->x_off + L->x_size;
        uint32_t need_y = (uint32_t)L->y_off + L->y_size;
        if (need_x <= plen_bits && need_y <= plen_bits) {
            acc_dx += extract_signed(p, L->x_off, L->x_size);
            acc_dy += extract_signed(p, L->y_off, L->y_size);
        }
    }
    if (L->btn_count && (!L->has_report_id || rid == L->btn_report_id)) {
        if ((uint32_t)L->btn_off + L->btn_count <= plen_bits) {
            s->mouse_buttons = (uint8_t)extract_bits(p, L->btn_off, L->btn_count);
        }
    }
    critical_section_exit(&hid_lock);
    adapter_led_notify_activity();
}

//--------------------------------------------------------------------
// TinyUSB Host Callbacks (run on Core 1)
//--------------------------------------------------------------------
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    mouse_layout_t lay;
    bool parsed = parse_mouse_desc(desc_report, desc_len, &lay);

    bool is_kbd = (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD);
    if (!is_kbd && desc_report && desc_len > 4) {
        for (uint16_t i = 0; i + 3 < desc_len; i++) {
            if (desc_report[i] == 0x05 && desc_report[i+1] == 0x01 &&
                desc_report[i+2] == 0x09 && desc_report[i+3] == 0x06) {
                is_kbd = true;
                break;
            }
        }
    }

    bool is_mouse = (itf_protocol == HID_ITF_PROTOCOL_MOUSE) ||
                    (parsed && lay.is_mouse);
    if (!is_kbd && !is_mouse) {
#if ENABLE_UART_DEBUG
        printf("[USB-H] Mount ignored: dev=%d inst=%d (proto=%d)\n",
               dev_addr, instance, itf_protocol);
#endif
        return;
    }

    critical_section_enter_blocking(&hid_lock);
    slot_t *s = find_slot(dev_addr, instance);
    if (!s) {
        for (int i = 0; i < CFG_TUH_HID; i++) {
            if (!slots[i].used) {
                s = &slots[i];
                break;
            }
        }
    }
    if (s) {
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->dev_addr = dev_addr;
        s->instance = instance;
        s->itf_protocol = is_kbd ? HID_ITF_PROTOCOL_KEYBOARD : HID_ITF_PROTOCOL_MOUSE;
        if (is_mouse && lay.valid) {
            s->use_layout = true;
            s->layout = lay;
        }
    }
    critical_section_exit(&hid_lock);
    if (!s) return;

    update_device_count();

#if ENABLE_UART_DEBUG
    printf("[USB-H] MOUNTED dev=%d inst=%d: %s (layout_valid=%d)\n",
           dev_addr, instance, is_kbd ? "KEYBOARD" : "MOUSE", lay.valid);
#endif

    if (is_kbd || !s->use_layout) {
        if (tuh_hid_get_protocol(dev_addr, instance) != HID_PROTOCOL_BOOT) {
            tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
        }
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    critical_section_enter_blocking(&hid_lock);
    slot_t *s = find_slot(dev_addr, instance);
    if (s) memset(s, 0, sizeof(*s));
    critical_section_exit(&hid_lock);

    update_device_count();

#if ENABLE_UART_DEBUG
    printf("[USB-H] UNMOUNTED dev=%d inst=%d\n", dev_addr, instance);
#endif
}

static bool valid_boot_kb_report(uint8_t const *report, uint16_t len) {
    if (len != 8) return false;
    for (int k = 2; k < 8; k++) {
        uint8_t kc = report[k];
        if (kc != 0 && (kc < HID_KEY_A || kc > HID_KEY_GUI_RIGHT)) return false;
    }
    return true;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    slot_t *s = find_slot(dev_addr, instance);
    if (s && len > 0) {
        if (s->itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
            if (tuh_hid_get_protocol(dev_addr, instance) == HID_PROTOCOL_BOOT &&
                valid_boot_kb_report(report, len)) {
                critical_section_enter_blocking(&hid_lock);
                memcpy(&s->kb, report, sizeof(hid_keyboard_report_t));
                critical_section_exit(&hid_lock);
                adapter_led_notify_activity();
            }
        } else if (s->use_layout) {
            handle_layout_mouse(s, report, len);
        } else if (tuh_hid_get_protocol(dev_addr, instance) == HID_PROTOCOL_BOOT &&
                   len >= 3 && len <= 8) {
            hid_mouse_report_t const *m = (hid_mouse_report_t const *)report;
            critical_section_enter_blocking(&hid_lock);
            s->mouse_buttons = m->buttons;
            acc_dx += m->x;
            acc_dy += m->y;
            critical_section_exit(&hid_lock);
            adapter_led_notify_activity();
        }
    }
    tuh_hid_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// State Aggregation -> Pro Controller State (Called on Core 0 every 15 ms)
//--------------------------------------------------------------------
static uint16_t clamp_stick_12(int val) {
    if (val < STICK_MIN) return STICK_MIN;
    if (val > STICK_MAX) return STICK_MAX;
    return (uint16_t)val;
}

static float clampf_val(float v, float min_v, float max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

void usb_host_get_procon_state(ProconState *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));

    bool up = false, down = false, left = false, right = false;
    bool cam_up = false, cam_down = false, cam_left = false, cam_right = false;
    uint8_t combined_mouse_buttons = 0;
    int32_t dx = 0, dy = 0;

    critical_section_enter_blocking(&hid_lock);
    dx = acc_dx;
    dy = acc_dy;
    acc_dx = 0;
    acc_dy = 0;

    for (int i = 0; i < CFG_TUH_HID; i++) {
        if (!slots[i].used) continue;

        if (slots[i].itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
            uint8_t mods = slots[i].kb.modifier;
            if (mods & KEYBOARD_MODIFIER_LEFTSHIFT)  st->btn[1] |= PROCON_BTN1_LCLICK; // L3
            if (mods & KEYBOARD_MODIFIER_LEFTCTRL)   st->btn[1] |= PROCON_BTN1_RCLICK; // R3

            for (int k = 0; k < 6; k++) {
                uint8_t key = slots[i].kb.keycode[k];
                if (key == 0) continue;

                switch (key) {
                    // Face buttons (Supports both PC intuitives Q/E/Space and NXIC L/K/I/J)
                    case KEY_L:
                    case KEY_Q:     st->btn[0] |= PROCON_BTN0_A; break;
                    case KEY_K:
                    case KEY_SPACE: st->btn[0] |= PROCON_BTN0_B; break;
                    case KEY_I:
                    case KEY_R:     st->btn[0] |= PROCON_BTN0_X; break;
                    case KEY_J:
                    case KEY_E:     st->btn[0] |= PROCON_BTN0_Y; break;

                    // Left Stick (WASD)
                    case KEY_W: up = true; break;
                    case KEY_S: down = true; break;
                    case KEY_A: left = true; break;
                    case KEY_D: right = true; break;

                    // Camera Pans / Right Stick (Arrow Keys)
                    case KEY_UP:    cam_up = true; break;
                    case KEY_DOWN:  cam_down = true; break;
                    case KEY_LEFT:  cam_left = true; break;
                    case KEY_RIGHT: cam_right = true; break;

                    // D-Pad (F/C/V/B)
                    case KEY_F: st->btn[2] |= PROCON_BTN2_UP; break;
                    case KEY_V:
                    case KEY_B: st->btn[2] |= PROCON_BTN2_DOWN; break;
                    case KEY_C: st->btn[2] |= PROCON_BTN2_LEFT; break;

                    // Bumpers & Triggers
                    case KEY_P: st->btn[0] |= PROCON_BTN0_ZR; break;
                    case KEY_O: st->btn[2] |= PROCON_BTN2_ZL; break;

                    // System
                    case KEY_T:
                    case KEY_TAB: st->btn[1] |= PROCON_BTN1_MINUS; break;
                    case KEY_U:
                    case KEY_ESC:
                    case KEY_ENTER: st->btn[1] |= PROCON_BTN1_PLUS; break;
                    case KEY_H:   st->btn[1] |= PROCON_BTN1_HOME; break;
                    case KEY_G:   st->btn[1] |= PROCON_BTN1_CAPTURE; break;
                    default: break;
                }
            }
        } else if (slots[i].itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
            combined_mouse_buttons |= slots[i].mouse_buttons;
        }
    }
    critical_section_exit(&hid_lock);

    // Mouse Buttons
    if (combined_mouse_buttons & MOUSE_BUTTON_LEFT)     st->btn[0] |= PROCON_BTN0_ZR;
    if (combined_mouse_buttons & MOUSE_BUTTON_RIGHT)    st->btn[2] |= PROCON_BTN2_ZL;
    if (combined_mouse_buttons & MOUSE_BUTTON_MIDDLE)   st->btn[1] |= PROCON_BTN1_RCLICK; // R3
    if (combined_mouse_buttons & MOUSE_BUTTON_BACKWARD) st->btn[2] |= PROCON_BTN2_L;
    if (combined_mouse_buttons & MOUSE_BUTTON_FORWARD)  st->btn[0] |= PROCON_BTN0_R;

    // Left Stick calculation
    uint16_t lx = STICK_CENTER;
    uint16_t ly = STICK_CENTER;
    if (left && !right) lx = STICK_MIN;
    else if (right && !left) lx = STICK_MAX;
    if (down && !up) ly = STICK_MIN;
    else if (up && !down) ly = STICK_MAX;
    procon_pack_stick(&st->stick[0], lx, ly);

    // Right Stick calculation (Mouse delta sweeps camera + Arrow key fallback)
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (dx != 0 || dy != 0) {
        last_mouse_move_time_ms = now;
        current_rx = clamp_stick_12(STICK_CENTER + (dx * MOUSE_STICK_SENSITIVITY));
        current_ry = clamp_stick_12(STICK_CENTER - (dy * MOUSE_STICK_SENSITIVITY));
    } else if (now - last_mouse_move_time_ms > MOUSE_IDLE_TIMEOUT_MS) {
        current_rx = STICK_CENTER;
        current_ry = STICK_CENTER;
    }

    uint16_t final_rx = current_rx;
    uint16_t final_ry = current_ry;
    if (cam_left && !cam_right) final_rx = STICK_MIN;
    else if (cam_right && !cam_left) final_rx = STICK_MAX;
    if (cam_down && !cam_up) final_ry = STICK_MIN;
    else if (cam_up && !cam_down) final_ry = STICK_MAX;

    procon_pack_stick(&st->stick[3], final_rx, final_ry);

    // -----------------------------------------------------------------
    // IMU 6-Axis Gyro Synthesis (Enables gyro aiming in Splatoon / Zelda / Fortnite)
    // -----------------------------------------------------------------
    const float yaw_deg_per_count   = 360.0f / GYRO_COUNTS_PER_360;
    const float pitch_deg_per_count = 360.0f / PITCH_COUNTS_PER_360;
    pend_yaw   -= (float)dx * yaw_deg_per_count;
    pend_pitch -= (float)dy * pitch_deg_per_count;

    const float lsb_dps  = 0.070f;
    const float frame_dt = 0.005f;
    const float game_dt  = 3.0f * frame_dt;
    const float max_deg  = 32000.0f * lsb_dps * game_dt;

    float take_yaw   = clampf_val(pend_yaw,   -max_deg, max_deg);
    float take_pitch = clampf_val(pend_pitch, -max_deg, max_deg);

    int16_t raw_yaw   = (int16_t)lrintf(take_yaw   / game_dt / lsb_dps);
    int16_t raw_pitch = (int16_t)lrintf(take_pitch / game_dt / lsb_dps);

    pend_yaw   -= (float)raw_yaw   * lsb_dps * game_dt;
    pend_pitch -= (float)raw_pitch * lsb_dps * game_dt;

    for (int f = 0; f < 3; f++) {
        // Accelerometer: 1G resting on Z (flat orientation)
        st->imu[f][0] = 0;
        st->imu[f][1] = 0;
        st->imu[f][2] = 4096;

        // Gyro angular velocities
        st->imu[f][3] = 0;
        st->imu[f][4] = -raw_pitch;
        st->imu[f][5] = raw_yaw;
    }

#if ENABLE_UART_DEBUG
    static uint32_t last_log_ms = 0;
    if ((dx != 0 || dy != 0 || combined_mouse_buttons != 0) && (now - last_log_ms >= 200)) {
        last_log_ms = now;
        printf("[INPUT] Mouse: dx=%ld dy=%ld btn=0x%02X | Stick L(%u,%u) R(%u,%u)\n",
               (long)dx, (long)dy, combined_mouse_buttons, lx, ly, final_rx, final_ry);
    }
#endif
}

//--------------------------------------------------------------------
// Core 1 main task for USB Host (Pico-PIO-USB)
//--------------------------------------------------------------------
static pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;

void usb_host_core1_task(void) {
    // 10 ms delay allows Core 0 to complete hardware setup before Core 1 starts PIO-USB
    sleep_ms(10);

    pio_cfg.pin_dp = PIN_USB_HOST_DP; // GP2 (D+), GP3 (D-)
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);

#if ENABLE_UART_DEBUG
    printf("\n=========================================\n");
    printf("[HOST] Pico-PIO-USB Host Initialized!\n");
    printf("[HOST] D+ Pin: GP%d (Pin %d), D- Pin: GP%d (Pin %d)\n",
           PIN_USB_HOST_DP, PIN_USB_HOST_DP == 2 ? 4 : PIN_USB_HOST_DP + 1,
           PIN_USB_HOST_DM, PIN_USB_HOST_DM == 3 ? 5 : PIN_USB_HOST_DM + 1);
    printf("=========================================\n\n");
#endif

    for (;;) {
        tuh_task();
    }
}

#endif // BACKEND_USB_HOST
