# Changelog

All notable changes to ReclaimPanel firmware are documented here.

---

## v1.8.0
- mDNS: unit reachable at reclampanel.local (or unitname.local) without remembering IP
- Update notification: web UI banner when a newer GitHub release is available
- Config backup: download all settings as a JSON file from the Network tab
- Config restore: upload a backup file to clone settings to another unit

## v1.7.2
- Renamed project to ReclaimPanel (removed all internal P16 references)
- WiFi AP renamed to ReclaimPanel-Setup, NVS namespace updated
- Full installer website (GitHub Pages) with browser-based flashing

## v1.7.1
- BCM cycle speed adjustable from web UI (Display tab slider, 100-3000 us)
- Live Hz readout shows refresh rate as you adjust
- bcmBaseUs persisted to flash on Save

## v1.7.0
- Web interface overhaul: charset UTF-8, no encoding issues
- Diagnostics tab: DDP frames/sec, display refreshes/sec, MAC address,
  WiFi channel, signal strength bar (color-coded), CPU speed, Flash size
- Debug log: 20-line ring buffer, enable/disable from web UI, /log endpoint
- Display tab: removed dead PWM levels selector, added BCM explanation
- Mode badge now correctly shows BCM ON / Binary
- Serial heartbeat includes rps (renders per second)
- Removed dead pwmLevels handling from save handler

## v1.6.1
- Brightness slider now updates panels live while dragging (oninput)
- Removed flash write from /brightness endpoint to prevent wear on every drag
- Brightness minimum lowered from 10% to 1%

## v1.6.0
- Binary Code Modulation (BCM) replaces broken temporal PWM
- 4-bit BCM: 16 levels per channel, 4096 total colors
- BCM cycles continuously in loop() at ~133 Hz - no frame gate needed
- fastClockPulse() with no delays (~200 ns vs ~2 us) enables 133 Hz at 4 panels
- Binary mode unchanged for instant single-frame response

## v1.5.3
- Expanded ddpBuffer from 1220 to 4820 bytes
- Handles case where FPP sends all 4 panels in one large DDP packet

## v1.5.2
- UDP drain loop moved to top of loop() before server.handleClient()
- Prevents socket buffer overflow when browser polls status during multi-panel frames

## v1.5.1
- Removed per-packet Serial.printf from processDDP
- At 115200 baud, printing 4ms per line blocked UDP socket between packets

## v1.5.0
- Combined 40x40 xLights model for 4-panel 2x2 grid
- Web UI: panel grid layout configurator with front-view diagram
- combinedBitToPixel lookup table maps hardware bits to combined buffer positions
- Multi-panel shift register chaining: reverse-order clock, single latch

## v1.4.0
- Multi-panel shift register chaining protocol fixed
- Reverse-order clocking (panel N-1 first), one latch at end
- Chase effect spans all panels

## v1.3.0
- Initial working single-panel release
- DDP reception, push-flag gating, web interface, OTA, chase test
