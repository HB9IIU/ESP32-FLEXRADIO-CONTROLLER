# FlexControl WiFi – ESP32 SmartSDR Tuning Knob

**FlexControl WiFi** is an ESP32-based network controller for **FlexRadio SmartSDR**.  
It talks to the SmartSDR CAT server over TCP (port 5002) and gives you a **hardware tuning knob** with:

- Main VFO encoder (with acceleration)
- Filter preset encoder (0–7)
- Volume encoder with mute
- Touch buttons for frequency presets, PTT and TUNE
- Mode cycling (USB / LSB )
- Status LEDs for CAT link and TX
- Captive-portal Wi-Fi configuration + OTA firmware updates

👉 All code and 3D enclosure files are **free and open source**.

## Demo Video

<p align="center">
  <a href="https://www.youtube.com/watch?v=9js4Cu4pR5Q" target="_blank">
    <img src="https://img.youtube.com/vi/9js4Cu4pR5Q/hqdefault.jpg" 
         alt="FlexControl WiFi Demo" width="480">
  </a>
</p>

## Features

- 🌀 **Main tuning encoder**  
  - High-resolution quadrature decoder in ISR  
  - Frequency acceleration based on tuning speed  
  - Periodic `FA;` polling to keep in sync with SmartSDR

- 🎚 **Filter encoder**  
  - 8 filter presets: CAT command `ZZFI00`…`ZZFI07`  
  - Encoder steps mapped to filter index 0–7

- 🔊 **Volume encoder + mute**  
  - Volume 0–100% via `ZZAGnnn;`  
  - Mute / unmute on volume encoder click (remembers last volume)  
  - Auto-unmute if you turn the volume knob while muted

- 🧲 **Touch buttons (TTP223 modules)**  
  - Touch 1: FT8 @ **40 m** preset (7.077 MHz, LSB)  
  - Touch 2: FT8 @ **20 m** preset (14.074 MHz, USB)  
  - Touch 3: **PTT** (press & hold = TX, release = RX)  
  - Touch 4: **TUNE** (short carrier, low power, configurable)  
  - Touch 5: **Mode cycle** (USB ↔ LSB, extendable)

- 🧠 **Smart CAT handling**  
  - Auto-discovery of the SmartSDR CAT server on LAN  
  - Caching of last known CAT host in NVS (`Preferences`)  
  - Transparent handling of modes (MDn;), power (ZZPC), PTT (ZZTX)

- 🌐 **Wi-Fi captive portal + OTA**  
  - First-boot captive portal to capture Wi-Fi credentials  
  - Web console logger (serial-over-web)  
  - OTA updates via ArduinoOTA helper

- 🔁 **Factory reset**  
  - Press and hold factory reset button at boot to clear Wi-Fi / NVS  
  - Red LED indicates reset activity

---

## Hardware

- **ESP32 board** (classic ESP32 DevKit-style with GPIOs 32/33 etc.)
- Main VFO: high-resolution E38 optical encoder (600 PPR, NPN, powered at 5 V with A/B pulled up to 3.3 V on the ESP32).
- Filter & volume: two HW-040 mechanical encoder modules with integrated push switches (volume switch also used for mute / factory reset).
- 5× **TTP223 touch sensors** (or similar)
- 2× LEDs (GREEN, RED) for status and TX indication
- 1× **factory-reset / volume encoder** push button
- 3D-printed enclosure (STL/STEP files in this repo)

> 🛠 All 3D files for the enclosure are included in the repository.  
> Check the `doc/` folder for STEP/STL files.

---

## Wiring

Below is the wiring as used in the firmware (`main.cpp`).

### LEDs

| Function          | ESP32 GPIO | Notes                           |
| ----------------- | ---------- | --------------------------------|
| GREEN status LED  | **13**     | CAT connected / mute indication |
| RED status / TX   | **4**      | TX, errors, blinking, reset LED |

> Both the “red LED” and the “reset LED” share GPIO 4 in the code.

### Rotary Encoders

All encoder A/B signals are read with internal pull-ups (`INPUT_PULLUP`).  
Encoder common pins go to **GND**.

#### Main VFO Encoder (frequency)

| Signal     | ESP32 GPIO | Notes                          |
| ---------- | ---------- | ------------------------------ |
| Encoder A  | **32**     | Quadrature A                   |
| Encoder B  | **33**     | Quadrature B                   |
| Push (BW)  | **16**     | Active-LOW, internal pull-up   |

#### Filter Encoder

| Signal    | ESP32 GPIO | Notes                                    |
| --------- | ---------- | ---------------------------------------- |
| Encoder A | **32**     | Quadrature A                             |
| Encoder B | **33**     | Quadrature B                             |
| Push      | **16**     | Active-LOW, used to reboot the ESP32     |

#### Volume Encoder

| Signal     | ESP32 GPIO | Notes                                             |
| ---------- | ---------- | ------------------------------------------------- |
| Encoder A  | **14**     | Quadrature A                                      |
| Encoder B  | **27**     | Quadrature B                                      |
| Push       | **17**     | Active-LOW, internal pull-up (also used for **factory reset** at boot) |

> Long-press on boot = factory reset (via `HB9IIUPortal::checkFactoryReset()`),  
> short press in normal operation = **mute / unmute**.

### Touch Buttons (TTP223)

TTP223 modules are wired as:

- `OUT` → corresponding GPIO (below)  
- `VCC` → 3.3 V  
- `GND` → GND

The firmware uses `INPUT_PULLDOWN`, with **touch = HIGH**, **idle = LOW**.

| Touch # | ESP32 GPIO | Function in firmware               |
| ------- | ---------- | ---------------------------------- |
| 1       | **23**     | FT8 40 m preset (7.077 MHz, LSB)   |
| 2       | **22**     | FT8 20 m preset (14.074 MHz, USB)  |
| 3       | **21**     | PTT (press = TX, release = RX)     |
| 4       | **19**     | TUNE (short carrier, low power)    |
| 5       | **18**     | Mode cycle (USB ↔ LSB)             |

### Factory Reset

| Signal                 | ESP32 GPIO | Notes                                  |
| ---------------------- | ---------- | -------------------------------------- |
| Factory reset button   | **17**     | Same physical button as volume push    |
| Factory reset LED      | **4**      | Same red LED as TX / status            |

At boot, `HB9IIUPortal::checkFactoryReset()` inspects this button to decide whether to erase NVS / Wi-Fi credentials.

---

## Firmware Overview

### Wi-Fi & Captive Portal

The project uses a custom helper:

- `HB9IIUPortal::begin()`  
  - Connects to stored Wi-Fi credentials if available  
  - Otherwise starts an **Access Point + captive portal** (`HB9IIUportalConfigurator.h`)  
  - Shows a status message in the web console

---
### 3D Renderings

<table>
  <tr>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_1.png" width="24%"></td>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_2.png" width="24%"></td>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_3.png" width="24%"></td>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_4.png" width="24%"></td>
  </tr>
  <tr>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_5.png" width="24%"></td>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_6.png" width="24%"></td>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_7.png" width="24%"></td>
    <td><img src="doc/3D_Renderings/HB9IIU_FlexController_Rendering_8.png" width="24%"></td>
  </tr>
</table>

## Hardware Preview

![HB9IIU FlexControl WiFi](https://github.com/HB9IIU/ESP32-FLEXRADIO-CONTROLLER/blob/main/doc/pictures/HB9IIU_FlexController_As_Build_2.png)

---

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, modify, and redistribute this software, including in commercial products, as long as any distributed derivative works are also licensed under GPL-3.0 and accompanied by their complete corresponding source code.

See the [LICENSE](./LICENSE) file for the full text.
