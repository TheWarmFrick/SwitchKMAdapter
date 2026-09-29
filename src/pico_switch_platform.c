#include <stdio.h>
#include <string.h>

#include <pico/cyw43_arch.h>
#include <pico/multicore.h>
#include <pico/async_context.h>
#include <uni.h>
#include "pico/time.h"

#include "sdkconfig.h"
#include "uni_hid_device.h"
#include "uni_log.h"
#include "usb.h"
#include "report.h"
#include "SwitchDescriptors.h"
#include "KeyboardKeys.h"

// Sanity check
#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

// 12-bit analog stick sensitivity & timing constants
#define MOUSE_STICK_SENSITIVITY 40
#define MOUSE_IDLE_TIMEOUT_MS 35

static uint32_t last_mouse_move_time_ms = 0;
static uint16_t current_rx = STICK_CENTER;
static uint16_t current_ry = STICK_CENTER;

static ProconIdxState idx_state;
static uint8_t connected_controllers = 0;

typedef struct {
    bool has_keyboard;
    bool has_mouse;
    uni_keyboard_t keyboard;
    uni_mouse_t mouse;
} CombinedControllerState;

static CombinedControllerState combined_states[CONFIG_BLUEPAD32_MAX_DEVICES];

//--------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------
static void empty_procon_state(ProconState *st) {
    memset(st, 0, sizeof(ProconState));
    procon_pack_stick(&st->stick[0], STICK_CENTER, STICK_CENTER);
    procon_pack_stick(&st->stick[3], STICK_CENTER, STICK_CENTER);

    // Resting flat on table: Accel Z = +4096 (1G), Gyro = 0
    for (int f = 0; f < 3; f++) {
        st->imu[f][0] = 0;
        st->imu[f][1] = 0;
        st->imu[f][2] = 4096;
        st->imu[f][3] = 0;
        st->imu[f][4] = 0;
        st->imu[f][5] = 0;
    }
}

static uint16_t clamp_stick_12(int val) {
    if (val < STICK_MIN) return STICK_MIN;
    if (val > STICK_MAX) return STICK_MAX;
    return (uint16_t)val;
}

//--------------------------------------------------------------------
// Keyboard Input -> Pro Controller State
//--------------------------------------------------------------------
static void fill_procon_from_keyboard(ProconState *st, const uni_keyboard_t *kb) {
    // Modifier keys
    if (kb->modifiers & UNI_KEYBOARD_MODIFIER_LEFT_SHIFT) {
        st->btn[1] |= PROCON_BTN1_LCLICK; // L3
    }
    if (kb->modifiers & UNI_KEYBOARD_MODIFIER_LEFT_CONTROL) {
        st->btn[1] |= PROCON_BTN1_RCLICK; // R3
    }

    bool up = false, down = false, left = false, right = false;
    bool cam_up = false, cam_down = false, cam_left = false, cam_right = false;

    for (int i = 0; i < UNI_KEYBOARD_PRESSED_KEYS_MAX; i++) {
        uint8_t key = kb->pressed_keys[i];
        if (key == 0) continue;

        switch (key) {
            // Face buttons
            case KEY_Q:
                st->btn[0] |= PROCON_BTN0_A;
                break;
            case KEY_SPACE:
                st->btn[0] |= PROCON_BTN0_B;
                break;
            case KEY_R:
                st->btn[0] |= PROCON_BTN0_X;
                break;
            case KEY_E:
                st->btn[0] |= PROCON_BTN0_Y;
                break;

            // D-Pad
            case KEY_B:
                st->btn[2] |= PROCON_BTN2_DOWN;
                break;
            case KEY_F:
                st->btn[2] |= PROCON_BTN2_UP;
                break;
            case KEY_I:
                st->btn[2] |= PROCON_BTN2_RIGHT;
                break;

            // System buttons
            case KEY_TAB:
                st->btn[1] |= PROCON_BTN1_MINUS;
                break;
            case KEY_ESC:
                st->btn[1] |= PROCON_BTN1_PLUS;
                break;
            case KEY_H:
                st->btn[1] |= PROCON_BTN1_HOME;
                break;
            case KEY_C:
                st->btn[1] |= PROCON_BTN1_CAPTURE;
                break;

            // Left Joystick (WASD)
            case KEY_W:
                up = true;
                break;
            case KEY_S:
                down = true;
                break;
            case KEY_A:
                left = true;
                break;
            case KEY_D:
                right = true;
                break;

            // Right Joystick Camera Sweeps (Arrow Keys)
            case KEY_UP:
                cam_up = true;
                break;
            case KEY_DOWN:
                cam_down = true;
                break;
            case KEY_LEFT:
                cam_left = true;
                break;
            case KEY_RIGHT:
                cam_right = true;
                break;

            default:
                break;
        }
    }

    // Left Stick calculation
    uint16_t lx = STICK_CENTER;
    uint16_t ly = STICK_CENTER;
    if (left && !right) lx = STICK_MIN;
    else if (right && !left) lx = STICK_MAX;

    if (down && !up) ly = STICK_MIN;
    else if (up && !down) ly = STICK_MAX;

    procon_pack_stick(&st->stick[0], lx, ly);

    // Keyboard camera sweep override for right stick
    if (cam_left || cam_right || cam_up || cam_down) {
        if (cam_left && !cam_right) current_rx = STICK_MIN;
        else if (cam_right && !cam_left) current_rx = STICK_MAX;

        if (cam_down && !cam_up) current_ry = STICK_MIN;
        else if (cam_up && !cam_down) current_ry = STICK_MAX;
    }
}

//--------------------------------------------------------------------
// Mouse Input -> Pro Controller State
//--------------------------------------------------------------------
static void fill_procon_from_mouse(ProconState *st, const uni_mouse_t *mouse) {
    absolute_time_t now = get_absolute_time();
    uint32_t now_ms = to_ms_since_boot(now);

    // Right Click -> ZL
    if (mouse->buttons & MOUSE_BUTTON_RIGHT) {
        st->btn[2] |= PROCON_BTN2_ZL;
    }

    // Left Click -> ZR
    if (mouse->buttons & MOUSE_BUTTON_LEFT) {
        st->btn[0] |= PROCON_BTN0_ZR;
    }

    // Middle Click -> D-Pad Left
    if (mouse->buttons & MOUSE_BUTTON_MIDDLE) {
        st->btn[2] |= PROCON_BTN2_LEFT;
    }

    // Scroll Wheel: Up -> L, Down -> R
    if (mouse->scroll_wheel > 0) {
        st->btn[2] |= PROCON_BTN2_L;
        ((uni_mouse_t *)mouse)->scroll_wheel = 0;
    } else if (mouse->scroll_wheel < 0) {
        st->btn[0] |= PROCON_BTN0_R;
        ((uni_mouse_t *)mouse)->scroll_wheel = 0;
    }

    // Mouse Movement -> Right Stick (Phase 1)
    if (mouse->delta_x != 0 || mouse->delta_y != 0) {
        last_mouse_move_time_ms = now_ms;

        // Invert Y delta because HID positive is downwards, while Switch 12-bit stick 0xFFF is upwards
        int rx = STICK_CENTER + (mouse->delta_x * MOUSE_STICK_SENSITIVITY);
        int ry = STICK_CENTER - (mouse->delta_y * MOUSE_STICK_SENSITIVITY);

        current_rx = clamp_stick_12(rx);
        current_ry = clamp_stick_12(ry);
    } else {
        if ((now_ms - last_mouse_move_time_ms) >= MOUSE_IDLE_TIMEOUT_MS) {
            current_rx = STICK_CENTER;
            current_ry = STICK_CENTER;
        }
    }

    procon_pack_stick(&st->stick[3], current_rx, current_ry);
}

static void set_led_status(void) {
    if (connected_controllers == 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    } else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    }
}

//--------------------------------------------------------------------
// Bluepad32 Platform Callbacks
//--------------------------------------------------------------------
static void pico_switch_platform_init(int argc, const char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    logi("pico_switch_platform: init() Pro Controller mode\n");
    connected_controllers = 0;

    idx_state.idx = 0;
    empty_procon_state(&idx_state.state);
    set_global_procon_state(&idx_state);
}

static void pico_switch_platform_on_init_complete(void) {
    logi("pico_switch_platform: on_init_complete()\n");

    // Enable Bluetooth pairing
    uni_bt_enable_new_connections_unsafe(true);
    uni_bt_del_keys_unsafe();

    // Turn off LED until devices connect
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    logi("BLUEPAD: Pro Controller ready for keyboard and mouse\n");
    multicore_fifo_push_timeout_us(0, 100);
}

static void pico_switch_platform_on_device_connected(uni_hid_device_t *d) {
    logi("pico_switch_platform: device connected: %p\n", d);
}

static void pico_switch_platform_on_device_disconnected(uni_hid_device_t *d) {
    logi("pico_switch_platform: device disconnected: %p\n", d);

    if (connected_controllers > 0) {
        connected_controllers--;
    }
    set_led_status();

    idx_state.idx = 0;
    empty_procon_state(&idx_state.state);
    set_global_procon_state(&idx_state);
}

static uni_error_t pico_switch_platform_on_device_ready(uni_hid_device_t *d) {
    logi("pico_switch_platform: device ready: %p\n", d);

    connected_controllers++;
    set_led_status();
    return UNI_ERROR_SUCCESS;
}

static void pico_switch_platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl) {
    ARG_UNUSED(d);
    uint8_t idx = 0;
    CombinedControllerState *state = &combined_states[idx];

    if (ctl->klass == UNI_CONTROLLER_CLASS_KEYBOARD) {
        state->has_keyboard = true;
        state->keyboard = ctl->keyboard;
    } else if (ctl->klass == UNI_CONTROLLER_CLASS_MOUSE) {
        state->has_mouse = true;
        state->mouse = ctl->mouse;
    }

    empty_procon_state(&idx_state.state);

    if (state->has_keyboard) {
        fill_procon_from_keyboard(&idx_state.state, &state->keyboard);
    }

    if (state->has_mouse) {
        fill_procon_from_mouse(&idx_state.state, &state->mouse);
    } else {
        procon_pack_stick(&idx_state.state.stick[3], current_rx, current_ry);
    }

    idx_state.idx = idx;
    set_global_procon_state(&idx_state);
}

static const uni_property_t *pico_switch_platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void pico_switch_platform_on_oob_event(uni_platform_oob_event_t event, void *data) {
    ARG_UNUSED(event);
    ARG_UNUSED(data);
}

// Entry Point
struct uni_platform *get_my_platform(void) {
    static struct uni_platform plat = {
        .name = "PicoSwitch Pro Controller Platform",
        .init = pico_switch_platform_init,
        .on_init_complete = pico_switch_platform_on_init_complete,
        .on_device_connected = pico_switch_platform_on_device_connected,
        .on_device_disconnected = pico_switch_platform_on_device_disconnected,
        .on_device_ready = pico_switch_platform_on_device_ready,
        .on_oob_event = pico_switch_platform_on_oob_event,
        .on_controller_data = pico_switch_platform_on_controller_data,
        .get_property = pico_switch_platform_get_property,
    };
    return &plat;
}
