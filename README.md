# SwitchKMAdapter — Nintendo Switch Pro Controller Adapter

Turns a Raspberry Pi Pico W into a Bluetooth keyboard and mouse adapter for the Nintendo Switch and Nintendo Switch OLED / Dock, emulating an official **Nintendo Switch Pro Controller (VID 0x057E / PID 0x2009)**.

## Key Features
- **Official Pro Controller Emulation**: Connects to the Switch as a genuine Pro Controller (`0x057E:0x2009`), handling USB 0x80 handshakes, subcommands, and factory SPI calibration roms.
- **High-Precision 12-Bit Analog Sticks**: Left stick (WASD) and Right stick (Mouse movement + Arrow keys) operate on full 12-bit axis resolution (`0x000` to `0xFFF`, center `0x800`).
- **High Polling Rate**: Streams standard Pro Controller 0x30 input reports at 125 Hz (8 ms interval).
- **Automated GitHub Actions CI**: Every commit, tag, and pull request automatically builds `SwitchKMAdapter.uf2` using ARM GCC and Pico SDK. Ready-to-flash binaries are available in GitHub Actions artifacts and Releases.
- **Phase 2 Ready**: Protocol and reporting architecture ready for mouse-to-gyro 6-axis IMU fusion.

---

## Default Controls Mapping

### Keyboard to Controller
| Keyboard Key | Pro Controller Input |
| :--- | :--- |
| **W / A / S / D** | Left Analog Stick (12-bit Up / Left / Down / Right) |
| **Arrow Up / Down / Left / Right** | Right Analog Stick Camera Sweeps (12-bit) |
| **Q** | **A** Button |
| **Space** | **B** Button |
| **R** | **X** Button |
| **E** | **Y** Button |
| **F** | D-Pad **Up** |
| **B** | D-Pad **Down** |
| **I** | D-Pad **Right** |
| **Left Shift** | **L3** (Left Stick Click) |
| **Left Control** | **R3** (Right Stick Click) |
| **Tab** | **Minus (-)** Button |
| **Esc** | **Plus (+)** Button |
| **H** | **Home** Button |
| **C** | **Capture** Button |

### Mouse to Controller
| Mouse Input | Pro Controller Input |
| :--- | :--- |
| **Mouse Motion (X/Y)** | Right Analog Stick (12-bit aim with smooth idle decay) |
| **Left Click** | **ZR** (Trigger) |
| **Right Click** | **ZL** (Trigger) |
| **Middle Click** | D-Pad **Left** |
| **Scroll Wheel Up** | **L** (Shoulder button) |
| **Scroll Wheel Down** | **R** (Shoulder button) |

---

## Installation & Flashing

1. Download the latest `SwitchKMAdapter.uf2` from GitHub Actions artifacts or Releases.
2. Hold the **BOOTSEL** button on your Raspberry Pi Pico W and plug it into your computer via micro-USB.
3. Drag and drop `SwitchKMAdapter.uf2` onto the `RPI-RP2` USB drive.
4. Plug the Pico W into your Nintendo Switch dock using a USB data cable.
5. Put your Bluetooth keyboard and Bluetooth mouse into pairing mode. The Pico W's onboard LED will illuminate once connected.

---

## Building from GitHub Actions

This repository includes a pre-configured GitHub Actions workflow (`.github/workflows/build.yml`):
- Pushing to `main` or creating a tag (`v1.0.0`) automatically triggers a build on Ubuntu 24.04 with `gcc-arm-none-eabi`.
- The compiled `SwitchKMAdapter.uf2` file is attached to the workflow run artifacts for instant download.

---

## Local Building

### Prerequisites
1. Raspberry Pi Pico W board
2. CMake (3.13+) and `arm-none-eabi-gcc` toolchain
3. Pico SDK (1.5.1+)

### Build Commands
```bash
git clone --recursive https://github.com/TheWarmFrick/SwitchKMAdapter.git
cd SwitchKMAdapter
cmake -B build -DPICO_BOARD=pico_w
cmake --build build --target SwitchKMAdapter
```
The resulting `SwitchKMAdapter.uf2` will be generated in the `build/` directory.

---

## Roadmap
- [x] **Phase 1**: Official Nintendo Switch Pro Controller protocol emulation (`0x057E:0x2009`), 12-bit stick scaling, right stick camera sweeps, and automated GitHub Actions CI.
- [ ] **Phase 2**: Mouse-to-gyro 6-axis IMU sensor fusion (nxic-pico math model: angle-preserving accumulator, gravity vector tracking, 3-frame 5ms sub-sampling, pitch reset key).
