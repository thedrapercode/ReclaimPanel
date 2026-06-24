# ReclaimPanel

ESP32 firmware for driving salvaged P20 triangle RGB LED panels.

Developed by JD Labs for the Draper Family Light Show, Kenton Ohio.
All shows are free community events for charity.

---

## What This Is

Our charity show was gifted 60 older style p20 panels by a local Real-Estate company, we had to remove the old panels and frame so they could get a newer style p10 panels. When we started to tear these panels down only half of the sign was connected due to only having one working DBstar control board. This control board used a proprietary system that included a full remote pc inside the sign along with the control board. The real-estate company had to use XM Player on another computer to upload new slides and the process was very time consuming and not ideal for displaying images and text for properties. Once we started digging into the p20 panels and the control board we found it very difficult to find a replacement that was inexpensive, available to ship, and used any other setup other than XM Player. That is when I decided to go down the rabbit hole of controllers to use with the panels to then connect to FPP and be programmed by xLights. This proved difficult right away, I learned that the protocol was different from the newer panels which use the Hub75 and dedicated row address lines, two rows are driven simultaneously, and a scan pattern that multiplexes through rows really fast. All the current solutions by WLED, SmartMatrix, and the HUB75 ESP32 library don't cover these older panels which look similar by the connectors but has 5 independent 80-bit shift register chains, one per group of 4 rows all sharing a single CLK/LAT/OE. No row addressing, no multiplexing, just parallel shift registers...ugh!! There is no data sheet for these and there is no open source firmware so i had to sit down and reverse engineer to figure out what the pins do, how many bits per chain, the pattern of the boards, and the shared clock system. After countless hours and many....many versions and small updates it finally worked. So after i did all the time it really came down to the clock, literally, once i finally cracked the 80-bit clock with 1 latch it started working like magic. 

---

## Hardware

### Panels
- Model: P20 RGB triangle panels, possibly Grouptek brand but may be from another brand
- Size: 20x20 = 400 RGB triangles per panel (1 addressable RGB pixel per triangle)
- Protocol: 5 parallel 80-bit shift register chains sharing CLK / LAT / OE pins
- NOT HUB75...ugh - incompatible with all standard HUB75 drivers

### ESP32
- Model: ESP32 WROOM-32, 30-pin, D0WD-V3 rev3
- Power: USB for ESP32, dedicated 5V supply for panels
- Shared ground between ESP32 and 5V panel supply is required

### Wiring

See [docs/WIRING.md](docs/WIRING.md) for the full pin mapping table.

---

## Software Stack

```
xLights  -->  FPP (Falcon Player)  -->  ESP32  -->  Panels
(PC)          (Raspberry Pi)            (10.0.0.101)
```

- Protocol: DDP (Distributed Display Protocol) on UDP port 4048
- xLights builds the sequences
- FPP plays them and streams DDP to the ESP32
- ESP32 drives the panels directly via shift registers

---

## Features

- Full-color BCM (Binary Code Modulation) rendering - 4096 colors, ~133 Hz refresh
- Binary mode for instant on/off response with no overhead
- Up to 4 panels in any grid layout (configurable from web UI)
- Web interface: test colors, brightness, orientation, BCM speed, diagnostics
- OTA firmware updates over WiFi
- Chase test effect (ESP32-driven, no FPP required)
- Jump scare trigger input (GPIO 34) - I had to include this for my haunted house on Halloween
- DDP packet reception with push-flag gating
- Static IP configuration

---

## Quick Start

1. Flash `ReclaimPanel.ino` to an ESP32 WROOM-32 or similar ESP32 using the dedicated install webpage.

2. On first boot, connect to WiFi AP: `ReclaimPanel-Setup` / password `reclaim1`

3. Open browser to `192.168.4.1` and configure:
   - WiFi credentials
   - Static IP (default 10.0.0.101)
   - Number of panels and start channel

4. Wire panels per [docs/WIRING.md](docs/WIRING.md)

5. Configure xLights and FPP per [docs/XLIGHTS_FPP.md](docs/XLIGHTS_FPP.md)

---

## Panel Protocol

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for full reverse-engineering notes on the HUB16
shift register protocol used by these panels.

---

## License

MIT - see [LICENSE](LICENSE)

