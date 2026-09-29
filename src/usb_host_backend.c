// USB Host Input Backend using Pico-PIO-USB on Core 1.
// Supports USB Keyboards and Mice (direct or via USB Hub) on configurable GPIO pins (default: GP2 D+, GP3 D-).

#include "adapter_config.h"

#if (ADAPTER_INPUT_BACKEND == BACKEND_USB_HOST)

#include "usb_host_backend.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "pio_usb.h"
#include "tusb.h"

#include "report.h"
#include "SwitchDescriptors.h"
#include "KeyboardKeys.h"

// 12-bit analog stick sensitivity & timing constants
#define MOUSE_STICK_SENSITIVITY 40
#define MOUSE_IDLE_TIMEOUT_MS 35

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

static ProconIdxState idx_state;

void usb_host_init(void) {
    critical_section_init(&hid_lock);
    memset(slots, 0, sizeof(slots));
}

static slot_t *find_slot(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < CFG_TUH_HID; i++) {
        if (slots[i].used && slots[i].dev_addr == dev_addr && slots[i].instance == instance) {
            return &slots[i];
        }
    }
    return NULL;
}

//--------------------------------------------------------------------
// Minimal HID report descriptor parser for mice
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
    uint8_t xy_id = 0, btn_id = 0;

    memset(out, 0, sizeof(*out));
    if (!d || len == 0) return false;

    uint16_t pos = 0;
    while (pos < len) {
        uint8_t prefix = d[pos++];
        if (prefix == 0xFE) {
            if (pos >= len) break;
            pos += 1 + d[pos];
            continue;
        }
        uint8_t size = prefix & 0x03;
        if (size == 3) size = 4;
        if (pos + size > len) break;

        uint32_t data = 0;
        for (uint8_t i = 0; i < size; i++) data |= (uint32_t)d[pos++] << (i * 8);

        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag  = (prefix >> 4) & 0x0F;

        if (type == 1) { // Global
            switch (tag) {
                case 0x00: usage_page = (uint16_t)data; break;
                case 0x07: rsize = data; break;
                case 0x08: cur_id = (uint8_t)data; any_id = true; break;
                case 0x09: rcount = data; break;
                default: break;
            }
        } else if (type == 2) { // Local
            if (tag == 0x00 && nusages < 8) {
                usages[nusages++] = (uint16_t)data;
            }
        } else if (type == 0) { // Main
            if (tag == 0x08) { // Input
                uint16_t *cur = cursor_for(cur_id, ids, curs, &ncurs);
                if (cur && !(data & 0x01)) { // variable data
                    if (usage_page == 0x01) { // Generic Desktop
                        for (int i = 0; i < nusages && i < (int)rcount; i++) {
                            uint16_t u = usages[i];
                            uint16_t off = (uint16_t)(*cur + i * rsize);
                            if (u == 0x02) out->is_mouse = true;
                            if (u == 0x30) { // X
                                out->x_off = off;
                                out->x_size = (uint8_t)rsize;
                                xy_id = cur_id;
                            } else if (u == 0x31) { // Y
                                out->y_off = off;
                                out->y_size = (uint8_t)rsize;
                                xy_id = cur_id;
                            }
                        }
                    } else if (usage_page == 0x09 && out->btn_count == 0 && rsize == 1) {
                        out->btn_off = *cur;
                        out->btn_count = (uint8_t)(rcount < 8 ? rcount : 8);
                        btn_id = cur_id;
                    }
                }
                if (cur) *cur = (uint16_t)(*cur + rsize * rcount);
            }
            nusages = 0;
        }
    }

    out->has_report_id = any_id;
    out->xy_report_id = xy_id;
    out->btn_report_id = btn_id;
    out->valid = out->x_size >= 4 && out->x_size <= 16 &&
                 out->y_size >= 4 && out->y_size <= 16;
    return out->valid || out->is_mouse;
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
}

//--------------------------------------------------------------------
// TinyUSB Host Callbacks
//--------------------------------------------------------------------
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    mouse_layout_t lay;
    bool parsed = parse_mouse_desc(desc_report, desc_len, &lay);

    bool is_kbd = (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD);
    bool is_mouse = (itf_protocol == HID_ITF_PROTOCOL_MOUSE) ||
                    (parsed && lay.is_mouse);
    if (!is_kbd && !is_mouse) return;

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
        }
    }
    tuh_hid_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// Input Translation -> Pro Controller State
//--------------------------------------------------------------------
static uint16_t clamp_stick_12(int val) {
    if (val < STICK_MIN) return STICK_MIN;
    if (val > STICK_MAX) return STICK_MAX;
    return (uint16_t)val;
}

static void usb_host_update_procon(void) {
    ProconState st;
    memset(&st, 0, sizeof(st));

    // Baseline IMU: Resting flat on table (+4096 on Z, 0 gyro)
    for (int f = 0; f < 3; f++) {
        st.imu[f][0] = 0;
        st.imu[f][1] = 0;
        st.imu[f][2] = 4096;
        st.imu[f][3] = 0;
        st.imu[f][4] = 0;
        st.imu[f][5] = 0;
    }

    bool up = false, down = false, left = false, right = false;
    bool cam_up = false, cam_down = false, cam_left = false, cam_right = false;
    uint8_t combined_mouse_buttons = 0;
    int32_t dx = 0, dy = 0;

    critical_section_enter_blocking(&hid_lock);
    for (int i = 0; i < CFG_TUH_HID; i++) {
        if (!slots[i].used) continue;

        if (slots[i].itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
            uint8_t mods = slots[i].kb.modifier;
            if (mods & KEYBOARD_MODIFIER_LEFTSHIFT)  st.btn[1] |= PROCON_BTN1_LCLICK; // L3
            if (mods & KEYBOARD_MODIFIER_LEFTCTRL)   st.btn[1] |= PROCON_BTN1_RCLICK; // R3

            for (int k = 0; k < 6; k++) {
                uint8_t key = slots[i].kb.keycode[k];
                if (key == 0) continue;

                switch (key) {
                    // Face buttons
                    case KEY_Q:     st.btn[0] |= PROCON_BTN0_A; break;
                    case KEY_SPACE: st.btn[0] |= PROCON_BTN0_B; break;
                    case KEY_R:     st.btn[0] |= PROCON_BTN0_X; break;
                    case KEY_E:     st.btn[0] |= PROCON_BTN0_Y; break;

                    // Left Stick (WASD)
                    case KEY_W: up = true; break;
                    case KEY_S: down = true; break;
                    case KEY_A: left = true; break;
                    case KEY_D: right = true; break;

                    // Camera Pans (Arrow Keys)
                    case KEY_UP:    cam_up = true; break;
                    case KEY_DOWN:  cam_down = true; break;
                    case KEY_LEFT:  cam_left = true; break;
                    case KEY_RIGHT: cam_right = true; break;

                    // D-Pad
                    case KEY_F: st.btn[1] |= PROCON_BTN1_DUP; break;
                    case KEY_B: st.btn[1] |= PROCON_BTN1_DDOWN; break;
                    case KEY_I: st.btn[1] |= PROCON_BTN1_DRIGHT; break;

                    // System
                    case KEY_TAB: st.btn[1] |= PROCON_BTN1_MINUS; break;
                    case KEY_ESC: st.btn[1] |= PROCON_BTN1_PLUS; break;
                    case KEY_H:   st.btn[1] |= PROCON_BTN1_HOME; break;
                    case KEY_C:   st.btn[1] |= PROCON_BTN1_CAPTURE; break;
                    default: break;
                }
            }
        } else if (slots[i].itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
            combined_mouse_buttons |= slots[i].mouse_buttons;
        }
    }

    dx = acc_dx;
    dy = acc_dy;
    acc_dx = 0;
    acc_dy = 0;
    critical_section_exit(&hid_lock);

    // Mouse Buttons
    if (combined_mouse_buttons & MOUSE_BUTTON_LEFT)     st.btn[0] |= PROCON_BTN0_ZR;
    if (combined_mouse_buttons & MOUSE_BUTTON_RIGHT)    st.btn[0] |= PROCON_BTN0_ZL;
    if (combined_mouse_buttons & MOUSE_BUTTON_MIDDLE)   st.btn[1] |= PROCON_BTN1_DLEFT;
    if (combined_mouse_buttons & MOUSE_BUTTON_BACKWARD) st.btn[0] |= PROCON_BTN0_L;
    if (combined_mouse_buttons & MOUSE_BUTTON_FORWARD)  st.btn[0] |= PROCON_BTN0_R;

    // Left Stick calculation
    uint16_t lx = STICK_CENTER;
    uint16_t ly = STICK_CENTER;
    if (left && !right) lx = STICK_MIN;
    else if (right && !left) lx = STICK_MAX;
    if (down && !up) ly = STICK_MIN;
    else if (up && !down) ly = STICK_MAX;
    procon_pack_stick(&st.stick[0], lx, ly);

    // Right Stick calculation (Mouse delta + Arrow key fallback)
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

    procon_pack_stick(&st.stick[3], final_rx, final_ry);

    memcpy(&idx_state.state, &st, sizeof(st));
    set_global_procon_state(&idx_state);
}

// Core 1 main task for USB Host
void usb_host_core1_task(void) {
    sleep_ms(10);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);

    for (;;) {
        tuh_task();
        usb_host_update_procon();
    }
}

#endif // BACKEND_USB_HOST
