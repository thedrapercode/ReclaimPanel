# xLights and FPP Setup Guide

## Network Layout

| Device | IP | Role |
|---|---|---|
| FPP (Raspberry Pi 4) | 10.0.0.11 | Show player, streams DDP to ESP32 |
| ESP32 Panel Driver | 10.0.0.101 | Receives DDP, drives panels |
| xLights PC | varies | Sequence editor, uploads to FPP |

---

## xLights Setup

### Controller (Controllers Tab)

- Vendor: WLED (This works the best for now)
- Model: WLED
- Type: Generic ESP32
- Protocol: DDP
- IP Address: 10.0.0.101 (or your ESP32 static IP)
- Start Channel: match your panel start channel (configured in ESP32 web interface)
- Channel Count: 1200 per panel (1200 for 1 panel, 4800 for 4 panels)

### Matrix Model (Layout Tab)

For a single 20x20 panel:
- Model Type: Matrix
- Width: 20
- Height: 20
- String Count: 20
- Lights per String: 20
- String Type: RGB
- Start Location: Top Right
- Direction: Horizontal
- Zig Zag: unchecked (Don't Zig Zag)

For a 2x2 grid (4 panels):
- Width: 40
- Height: 40
- String Count: 40
- Lights per String: 40
- All other settings the same

### FPP Connect

Tools > FPP Connect
- Select your FPP instance
- UDP Out All
- Check: Sequences and Media
- Upload

---

## FPP Setup

### Output Configuration

In FPP: Outputs > Channel Outputs > Add

- Output Type: DDP (One Based)
- IP Address: 10.0.0.101 (ESP32)
- Start Channel: match xLights model start channel
- Channel Count: 1200 per panel (or total for all panels)
- Universe/Packet Size: 1200 (leave at 1200 even for multi-panel, FPP sends multiple packets per frame)

### Playback

- Upload sequences via xLights FPP Connect
- Create a playlist in FPP and add your sequences
- Enable loop if you want continuous playback
- Make sure Output is enabled (green status indicator)

---

## Troubleshooting

**FPP shows active but panels are dark**
- Verify start channel matches between xLights model, FPP output, and ESP32 web Interface
- Confirm ESP32 is on the network (ping you IP address)
- Check shared ground between ESP32 and panel 5V supply

**Only some panels light up**
- Verify panel count in ESP32 web UI matches physical panels connected
- Check ribbon cable connections (IN on left, OUT on right, from back)
- Confirm arrangement setting matches physical layout

**Colors are wrong on some panels**
- Try toggling Flip Rows or Flip Columns in the Display tab
- Verify BCM mode is enabled for full color (Display tab)

**Preview in xLights shows only half the content**
- This is a normal xLights behavior when effects were drawn on a smaller model
- Delete and re-add effects on the correctly sized model
- FPP playback is not affected by preview appearance
