/*
 * Nintendo Switch Pro Controller Protocol Emulation over USB.
 * References:
 *   - https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
 *   - https://github.com/mizuyoukanao/nxic-pico
 *
 * Flow:
 * 1. Switch sends 0x80 USB commands (handshake, MAC query).
 * 2. Switch sends 0x01 subcommand reports (device info, SPI flash reads for
 *    stick and IMU calibration).
 * 3. Once 0x80 0x04 arrives, adapter streams 0x30 full input reports every 8 ms.
 */

#include <string.h>
#include <stdio.h>
#include "pico/time.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "SwitchDescriptors.h"
#include "report.h"
#include "procon.h"

// Report interval: 8 ms (125 Hz)
#define REPORT_INTERVAL_US 8000

// Battery full + charging (0x90), wired connection (0x01) -> 0x91
#define BATTERY_CONN 0x91

static uint8_t mac[6];
static bool streaming = false;
static bool reply_pending = false;
static uint8_t reply_buf[64];
static absolute_time_t next_report;
static ProconIdxState last_idx_state;

//--------------------------------------------------------------------
// Emulated SPI flash, 0x6000-0x60FF (factory configuration/calibration).
//--------------------------------------------------------------------
static uint8_t spi_rom_6000[0x100];

static void spi_rom_init(void) {
    memset(spi_rom_6000, 0xFF, sizeof(spi_rom_6000));

    // 0x6020: IMU factory calibration
    static const uint8_t imu_cal[24] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // accel origin
        0x00, 0x40, 0x00, 0x40, 0x00, 0x40, // accel sensitivity (16384 = 8G)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // gyro origin
        0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34, // gyro sensitivity (13371 = 2000dps)
    };
    memcpy(&spi_rom_6000[0x20], imu_cal, sizeof(imu_cal));

    // 0x603D: left stick factory calibration (max-center / center / center-min),
    // 0x6046: right stick (center / center-min / max-center).
    // Center 0x800, +/-0x700 travel, packed as 12-bit pairs.
    static const uint8_t lstick_cal[9] = {
        0x00, 0x07, 0x70, 0x00, 0x08, 0x80, 0x00, 0x07, 0x70,
    };
    static const uint8_t rstick_cal[9] = {
        0x00, 0x08, 0x80, 0x00, 0x07, 0x70, 0x00, 0x07, 0x70,
    };
    memcpy(&spi_rom_6000[0x3D], lstick_cal, sizeof(lstick_cal));
    memcpy(&spi_rom_6000[0x46], rstick_cal, sizeof(rstick_cal));

    // 0x6050: body / button / grip colors (RGB)
    static const uint8_t colors[12] = {
        0x32, 0x32, 0x32, // body: dark gray
        0xFF, 0xFF, 0xFF, // buttons: white
        0xFF, 0xFF, 0xFF, // left grip: white
        0xFF, 0xFF, 0xFF, // right grip: white
    };
    memcpy(&spi_rom_6000[0x50], colors, sizeof(colors));

    // 0x6080: 6-axis horizontal offsets, followed by stick device parameters.
    // Accel report when controller is flat: (0, 0, +4096)
    static const uint8_t horizontal_offset[6] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    };
    static const uint8_t stick_params[18] = {
        0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54,
        0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
    };
    memcpy(&spi_rom_6000[0x80], horizontal_offset, sizeof(horizontal_offset));
    memcpy(&spi_rom_6000[0x86], stick_params, sizeof(stick_params));
    memcpy(&spi_rom_6000[0x98], stick_params, sizeof(stick_params));
}

static uint8_t spi_read_byte(uint32_t addr) {
    if (addr >= 0x6000 && addr < 0x6100) return spi_rom_6000[addr - 0x6000];
    return 0xFF;
}

// Input report timer ticks in 5 ms units
static uint8_t timer_byte(void) {
    return (uint8_t)(to_ms_since_boot(get_absolute_time()) / 5);
}

void procon_init(void) {
    spi_rom_init();

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    // Nintendo Switch OUI prefix + board-unique tail
    mac[0] = 0x7C;
    mac[1] = 0xBB;
    mac[2] = 0x8A;
    mac[3] = id.id[5];
    mac[4] = id.id[6];
    mac[5] = id.id[7];

    // Initial state: center sticks, 1G Z-accel
    memset(&last_idx_state, 0, sizeof(last_idx_state));
    procon_pack_stick(&last_idx_state.state.stick[0], STICK_CENTER, STICK_CENTER);
    procon_pack_stick(&last_idx_state.state.stick[3], STICK_CENTER, STICK_CENTER);
    for (int f = 0; f < 3; f++) {
        last_idx_state.state.imu[f][0] = 0;     // Accel X
        last_idx_state.state.imu[f][1] = 0;     // Accel Y
        last_idx_state.state.imu[f][2] = 4096;  // Accel Z (+1G flat)
        last_idx_state.state.imu[f][3] = 0;     // Gyro X
        last_idx_state.state.imu[f][4] = 0;     // Gyro Y
        last_idx_state.state.imu[f][5] = 0;     // Gyro Z
    }

    streaming = false;
    reply_pending = false;
    next_report = get_absolute_time();
}

bool procon_is_streaming(void) {
    return streaming;
}

//--------------------------------------------------------------------
// 0x80-prefixed USB commands
//--------------------------------------------------------------------
static void handle_usb_cmd(uint8_t const *buf, uint16_t len) {
    if (len < 2) return;
    memset(reply_buf, 0, sizeof(reply_buf));
    reply_buf[0] = 0x81;
    reply_buf[1] = buf[1];

    switch (buf[1]) {
        case 0x01: // status / MAC request
            reply_buf[2] = 0x00;
            reply_buf[3] = 0x03; // Pro Controller
            for (int i = 0; i < 6; i++) reply_buf[4 + i] = mac[5 - i];
            reply_pending = true;
            break;
        case 0x02: // handshake
        case 0x03: // baud rate ack
            reply_pending = true;
            break;
        case 0x04: // force USB HID only -> start streaming 0x30
            streaming = true;
            next_report = get_absolute_time();
            break;
        case 0x05: // allow timeout -> stop streaming
            streaming = false;
            break;
        default:
            break;
    }
}

//--------------------------------------------------------------------
// 0x01 rumble + subcommand reports, answered with 0x21 input report
//--------------------------------------------------------------------
static void handle_subcmd(uint8_t const *buf, uint16_t len) {
    uint8_t sub = (len > 10) ? buf[10] : 0x00;
    memset(reply_buf, 0, sizeof(reply_buf));
    reply_buf[0] = 0x21;
    reply_buf[1] = timer_byte();
    reply_buf[2] = BATTERY_CONN;
    memcpy(&reply_buf[3], last_idx_state.state.btn, 3);
    memcpy(&reply_buf[6], last_idx_state.state.stick, 6);
    reply_buf[12] = 0x80; // vibrator input report ack

    uint8_t ack = 0x80; // default ack
    uint8_t *d = &reply_buf[15];

    switch (sub) {
        case 0x01: // Bluetooth manual pairing
            ack = 0x81;
            d[0] = 0x03;
            break;
        case 0x02: // request device info
            ack = 0x82;
            d[0] = 0x03; // firmware 3.72
            d[1] = 0x48;
            d[2] = 0x03; // Pro Controller
            d[3] = 0x02;
            for (int i = 0; i < 6; i++) d[4 + i] = mac[i];
            d[10] = 0x01;
            d[11] = 0x02; // colors come from SPI
            break;
        case 0x03: // set input report mode
            if (len > 11 && buf[11] == 0x30) {
                streaming = true;
                next_report = get_absolute_time();
            }
            break;
        case 0x04: // trigger buttons elapsed time
            ack = 0x83;
            break;
        case 0x10: { // SPI flash read
            if (len < 16) break;
            ack = 0x90;
            uint32_t addr = (uint32_t)buf[11] | ((uint32_t)buf[12] << 8) |
                            ((uint32_t)buf[13] << 16) | ((uint32_t)buf[14] << 24);
            uint8_t n = buf[15];
            if (n > 0x1D) n = 0x1D;
            memcpy(d, &buf[11], 5); // echo address + length
            for (uint8_t i = 0; i < n; i++) d[5 + i] = spi_read_byte(addr + i);
            break;
        }
        case 0x21: { // set NFC/IR MCU configuration
            ack = 0xA0;
            static const uint8_t mcu_state[8] = {
                0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01,
            };
            memcpy(d, mcu_state, sizeof(mcu_state));
            break;
        }
        default:
            break;
    }

    reply_buf[13] = ack;
    reply_buf[14] = sub;
    reply_pending = true;
}

//--------------------------------------------------------------------
// TinyUSB device callbacks
//--------------------------------------------------------------------
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    if (bufsize == 0) return;

    switch (buffer[0]) {
        case 0x80:
            handle_usb_cmd(buffer, bufsize);
            break;
        case 0x01:
            handle_subcmd(buffer, bufsize);
            break;
        case 0x10: // rumble only (no reply required)
        default:
            break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_umount_cb(void) {
    streaming = false;
    reply_pending = false;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    streaming = false;
    reply_pending = false;
}

//--------------------------------------------------------------------
// Main Pro Controller USB Task (called on Core 0)
//--------------------------------------------------------------------
void procon_task(void) {
    if (!tud_hid_ready()) return;

    // Send pending handshake or subcommand replies first
    if (reply_pending) {
        reply_pending = false;
        tud_hid_report(0, reply_buf, sizeof(reply_buf));
        return;
    }

    if (!streaming) return;
    if (!time_reached(next_report)) return;

    next_report = make_timeout_time_us(REPORT_INTERVAL_US);

    // Fetch the latest global controller state produced by Bluepad32 on Core 1
    get_global_procon_state(&last_idx_state);

    uint8_t rpt[64];
    memset(rpt, 0, sizeof(rpt));
    rpt[0] = 0x30;            // Full standard input report
    rpt[1] = timer_byte();    // 5ms tick timer
    rpt[2] = BATTERY_CONN;    // Battery full + wired
    memcpy(&rpt[3], last_idx_state.state.btn, 3);
    memcpy(&rpt[6], last_idx_state.state.stick, 6);
    rpt[12] = 0x80;           // Vibrator input report ack
    memcpy(&rpt[13], last_idx_state.state.imu, sizeof(last_idx_state.state.imu)); // 36 bytes (3 frames x 12 bytes)

    tud_hid_report(0, rpt, sizeof(rpt));
}
