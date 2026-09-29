# SwitchKMAdapter — Nintendo Switch Pro Controller Adapter

Turns a Raspberry Pi Pico or Pico W into a high-performance keyboard and mouse adapter for the Nintendo Switch, Nintendo Switch OLED, and Nintendo Switch Dock, emulating an official **Nintendo Switch Pro Controller (VID 0x057E / PID 0x2009)**.

---

## ⚡ Key Features

- **Official Pro Controller Emulation**: Connects to the Switch as a genuine Pro Controller (`0x057E:0x2009`), handling USB 0x80 handshakes, subcommands, and factory SPI calibration ROM tables.
- **Selectable Input Backend**: Choose between **Wired USB Host** (zero-latency wired keyboards & mice on GPIO pins via Pico-PIO-USB) or **Wireless Bluetooth** (CYW43 Bluepad32 on Pico W).
- **Customizable USB Host Pins**: Defaults to **GP2 (D+)** and **GP3 (D-)**, fully configurable in `include/adapter_config.h` or via CMake.
- **High-Precision 12-Bit Analog Sticks**: Left stick (WASD) and Right stick (Mouse movement + Arrow keys) operate on full 12-bit axis resolution (`0x000` to `0xFFF`, center `0x800`).
- **High Polling Rate**: Streams standard Pro Controller 0x30 input reports at 125 Hz (8 ms interval).
- **Automated Dual-Target GitHub Actions CI**: Every commit builds and publishes both `SwitchKMAdapter-USB-Host.uf2` and `SwitchKMAdapter-Wireless-BT.uf2` ready to flash.

> [!IMPORTANT]
> **Nintendo Switch Setting Required:**
> On your Nintendo Switch console, you **must enable**:
> **System Settings $\rightarrow$ Controllers and Sensors $\rightarrow$ Pro Controller Wired Communication: ON**
> If this setting is turned OFF, the Nintendo Switch will not communicate with any wired Pro Controller over USB and will disconnect it immediately.

---

## 🔌 USB Host Hardware Wiring (Pico-PIO-USB)

When using the **USB Host** backend (`BACKEND_USB_HOST`), wire a standard USB-A female socket or OTG breakout cable to your Raspberry Pi Pico as follows:

| USB-A Female Pin | Wire Color | Raspberry Pi Pico Pin | Function |
| :--- | :--- | :--- | :--- |
| **Pin 1 (VBUS)** | Red | **Pin 40 (VBUS)** | 5V Power from Switch Dock |
| **Pin 2 (D-)** | White | **Pin 5 (GPIO 3)** | USB Data Minus (D-) |
| **Pin 3 (D+)** | Green | **Pin 4 (GPIO 2)** | USB Data Plus (D+) |
| **Pin 4 (GND)** | Black | **Pin 3 or 8 (GND)** | Ground |

*Tip: You can plug a multi-port USB hub into the USB-A port to connect both your keyboard and mouse simultaneously.*

---

## ⚙️ Configuration & Backend Selection

The input backend is configured in `include/adapter_config.h`:

```c
// Available Input Backend modes
#define BACKEND_WIRELESS_BT 1 // Wireless Bluetooth (CYW43 Bluepad32 on Pico W)
#define BACKEND_USB_HOST    2 // Wired USB Host (Pico-PIO-USB on RP2040 GPIO pins)

// Active Input Backend selection (default: USB Host)
#define ADAPTER_INPUT_BACKEND BACKEND_USB_HOST

// USB Host GPIO Pin Configuration
#define PIN_USB_HOST_DP 2 // GPIO 2 = USB D+ (Physical Pin 4)
#define PIN_USB_HOST_DM 3 // GPIO 3 = USB D- (Physical Pin 5)
```

To switch between backends via CMake during compilation:
```bash
# To build USB Host version (default):
cmake -B build -DPICO_BOARD=pico_w -DADAPTER_INPUT_BACKEND=USB_HOST

# To build Wireless Bluetooth version (Pico W only):
cmake -B build -DPICO_BOARD=pico_w -DADAPTER_INPUT_BACKEND=WIRELESS_BT
```

---

## 🎮 Default Controls Mapping

### Keyboard to Controller
| Keyboard Key | Pro Controller Input | Notes |
| :--- | :--- | :--- |
| **W / A / S / D** | Left Analog Stick | Full 12-bit (0x000 to 0xFFF) |
| **Arrow Up / Down / Left / Right** | Right Analog Stick Sweeps | Full camera panning |
| **Q** | **A** Button | Confirm / Interact |
| **Space** | **B** Button | Jump / Cancel |
| **R** | **X** Button | Map / Secondary |
| **E** | **Y** Button | Action / Reset |
| **F** | D-Pad **Up** | Ping / Quick Item |
| **B** | D-Pad **Down** | Emote / Super Call |
| **I** | D-Pad **Right** | Inventory / Switch |
| **Left Shift** | **L3** (Left Stick Click) | Sprint / Crouch |
| **Left Control** | **R3** (Right Stick Click) | Zoom / Melee |
| **Tab** | **Minus (-)** Button | Scoreboard / Map |
| **Esc** | **Plus (+)** Button | Pause / Options |
| **H** | **Home** Button | Switch Home Screen |
| **C** | **Capture** Button | Screenshot & 30s Video |

### Mouse to Controller
| Mouse Input | Pro Controller Input | Notes |
| :--- | :--- | :--- |
| **Mouse Motion (X/Y)** | Right Analog Stick | 12-bit aim with smooth idle decay (Phase 2: Gyro) |
| **Left Click** | **ZR** (Trigger) | Primary Fire / Attack |
| **Right Click** | **ZL** (Trigger) | Aim Down Sights / Guard |
| **Middle Click** | D-Pad **Left** | Quick Ping / Secondary |
| **Side Button 1** | **L** (Shoulder) | Sub-weapon / Tactical |
| **Side Button 2** | **R** (Shoulder) | Grenade / Special |

---

## 📦 Installation & Flashing

1. Download the firmware `.uf2` from GitHub Actions artifacts or Releases:
   - `SwitchKMAdapter-USB-Host.uf2` for wired USB keyboard & mouse.
   - `SwitchKMAdapter-Wireless-BT.uf2` for Bluetooth keyboard & mouse.
2. Hold the **BOOTSEL** button on your Raspberry Pi Pico/Pico W and connect it to your PC.
3. Drag and drop the `.uf2` file onto the `RPI-RP2` drive.
4. Connect the Pico to your Nintendo Switch dock using a USB-C data cable.

---

## 🔍 USB Host Debugging & Diagnostics

SwitchKMAdapter includes built-in hardware diagnostics (Onboard LED states and UART Serial Monitor) to verify USB input:

### 1. Onboard Status LED (GPIO 25)
The onboard LED provides instant visual feedback without needing any extra tools:

| LED Pattern | Meaning | Next Step |
| :--- | :--- | :--- |
| **Calm Heartbeat** (1 pulse / sec) | Standby mode. Pico is running, but no USB devices are detected on GP2/GP3. | Check OTG cable, 5V power, and verify mouse is plugged in. |
| **Rapid Double-Blink** (2 pulses / sec) | Nintendo Switch is connected and streaming, but **no keyboard or mouse is detected**. | Verify GP2 (D+) and GP3 (D-) wiring polarity. |
| **Solid ON** | **USB Device Mounted!** Pico-PIO-USB successfully enumerated your keyboard or mouse. | Ready to play. |
| **Flicker / Dip** | **Active Input Packets!** Toggles briefly whenever a key is pressed or mouse is moved. | Confirms reports are streaming into the Pro Controller engine. |

### 2. Live UART Serial Logging (GP0 @ 115200 Baud)
You can view real-time debug messages by connecting any USB-to-UART serial adapter (or FTDI/CH340 dongle) to the Pico:
- **TX Pin:** **GPIO 0** (Physical Pin 1 on Pico) $\rightarrow$ Connect to **RX** on your USB-to-Serial adapter.
- **GND Pin:** **Pin 3 or 8** $\rightarrow$ Connect to **GND** on your adapter.
- **Baud Rate:** `115200 8N1` (use PuTTY, minicom, or `picocom /dev/ttyUSB0 -b 115200`).

#### Example UART Live Output:
```text
==================================================
 SwitchKMAdapter Firmware Starting (USB Host Mode)
 - SysClock:  120 MHz
 - Host D+:   GP2 (Physical Pin 4)
 - Host D-:   GP3 (Physical Pin 5)
 - UART TX:   GP0 (Physical Pin 1) @ 115200 baud
 - Status LED: GP25
==================================================

[HOST] Pico-PIO-USB Host Initialized!
[HOST] MOUNTED dev=1 inst=0: MOUSE (layout_valid=1)
[SWITCH] Handshake 0x80/0x02 acked
[SWITCH] Set input mode 0x30 -> active!
[SWITCH] Started streaming 0x30 Pro Controller reports!
[INPUT] Mouse: dx=12 dy=-4 btn=0x01 | Stick L(2048,2048) R(2528,2208)
```

---

## 🗺️ Roadmap
- [x] **Phase 1**: Official Nintendo Switch Pro Controller protocol emulation (`0x057E:0x2009`), 12-bit stick scaling, right stick camera sweeps, and automated GitHub Actions CI.
- [x] **Input Backend Toggle**: Configurable Wired USB Host (Pico-PIO-USB on GP2/GP3) vs Wireless Bluetooth (CYW43 Bluepad32) with dual CI builds.
- [x] **Hardware Debugging & Diagnostics**: Built-in status LED state machine and live UART serial monitor at 115200 baud.
- [ ] **Phase 2**: Mouse-to-gyro 6-axis IMU sensor fusion (nxic-pico math model: angle-preserving accumulator, gravity vector tracking, 3-frame 5ms sub-sampling, pitch reset key).

