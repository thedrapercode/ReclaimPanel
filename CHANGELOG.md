# Changelog

All notable changes to ReclaimPanel firmware are documented here.

---

## v1.0 Stable
- Stable Relase Version after Beta Testing

## v1.8.0 Beta
- mDNS: ESP32 now reachable at reclampanel.local (or unitname.local) without remembering IP
- Update notification: web UI banner when a new GitHub release is available
- Config backup: download all settings as a JSON file from the Network tab
- Config restore: upload a backup file to clone settings to another unit

## v1.7.2 Beta
- Renamed project to ReclaimPanel 
- WiFi AP renamed to ReclaimPanel-Setup and NVS namespace updated
- Full installer website (GitHub Pages) with browser-based flashing

## v1.7.1 Beta
- BCM cycle speed adjustable from web UI (Display tab slider, 100-3000 us)
- Live readout shows refresh rate as you adjust
- bcmBaseUs persisted to flash on Save

## v1.7.0 Beta
- Web interface overhaul: charset UTF-8
- Diagnostics tab: DDP frames/sec, display refreshes/sec, MAC address,
  WiFi channel, signal strength bar (color coded), CPU speed, Flash size
- Debug log: 20-line ring buffer, enable/disable from web UI, /log endpoint
- Display tab: removed dead PWM levels selector, added BCM explanation
- Mode badge now correctly shows BCM ON and Binary
- Serial heartbeat includes renders per second
- Removed dead pwmLevels from save handler

## v1.6.1 Beta
- Brightness slider now updates panels live while dragging
- Removed flash write from brightness endpoint to prevent wear
- Brightness minimum lowered from 10% to 1%

## v1.6.0 Beta
- Binary Code Modulation (BCM) replaces broken PWM
- 4-bit BCM: 16 levels per channel, 4096 total colors
- BCM cycles continuously in loop() at ~133 Hz - no frame gate needed
- fastClockPulse() with no delays (~200 ns vs ~2 us) enables 133 Hz at 4 panels
- Binary mode unchanged for instant single frame response

## v1.5.3 Beta
- Expanded ddpBuffer from 1220 to 4820 bytes
- Handles case where FPP sends all 4 panels in one large DDP packet

## v1.5.2 Beta
- UDP drain loop moved to top of loop() before server.handleClient()
- Prevents socket buffer overflow when browser polls status during multi-panel frames

## v1.5.1 Beta
- Removed per packet Serial.printf from processDDP
- At 115200 baud, printing 4ms per line blocked UDP socket between packets

## v1.5.0 Beta
- Combined 40x40 xLights model for 4-panel 2x2 grid
- Web UI: panel grid layout editor with front view diagram
- combinedBitToPixel lookup table maps hardware bits to combined buffer positions
- Multi panel shift register chaining: reverse order clock and single latch

## v1.4.0 Beta
- Multi-panel shift register chaining protocol fixed
- Reverse order clocking (panel N-1 first) with one latch at end
- Chase effect spans all panels

## v1.3.0 Beta
- Initial working single-panel release
- DDP reception, push-flag gating, web interface, OTA, chase test
