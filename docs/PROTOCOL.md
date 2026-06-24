# HUB16 Panel Protocol

Reverse-engineered from Grouptek or similar brand P20 triangle RGB panels.
This protocol is NOT HUB75 and will NOT work with all standard HUB75 drivers.

---

## Physical Interface

- 20-pin ribbon cable (HUB16 style)
- 5 parallel data groups: each group has separate R, G, B data lines
- Shared single CLK, LAT, OE pins across all 5 groups

## Shift Register Architecture

- 5 groups (data chains), labeled group 0-4
- Each group: one 80-bit shift register chain (R), one for (G), one for (B)
- CLK clocks all 5 chains simultaneously on every pulse
- LAT latches all 5 chains simultaneously on every pulse
- OE (active low) enables/disables all output simultaneously

## Render Sequence Per Frame

```
For bit position 0 to 79:
    Set all 5 groups' R/G/B data lines for this bit position
    Pulse CLK once  (shifts all 5 chains by 1 bit)
Pulse LAT once  (latches all 5 chains' outputs simultaneously)
```

Total: 80 clock pulses, 1 latch pulse. One latch for the entire panel, not per group.

## Group to Row Mapping

| Group | Physical Rows |
|-------|--------------|
| 0     | 1 - 4        |
| 1     | 5 - 8        |
| 2     | 9 - 12       |
| 3     | 13 - 16      |
| 4     | 17 - 20      |

## Bit-to-Pixel Mapping

The 80 bits per group follow a serpentine pattern within 4-column blocks.

Each group covers 4 rows x 20 columns = 80 LEDs.
The 20 columns are divided into 5 blocks of 4 columns each (bits 0-15, 16-31, 32-47, 48-63, 64-79).

Within each 16-bit block:
- Bits 0-1: row 3, column 3 (rightmost of block)
- Bits 2-3: row 3, column 2
- Bits 4-5: row 3, column 1
- Bits 6-7: row 3, column 0 (leftmost of block)
- Bits 8-9: row 0, column 0
- Bits 10-11: row 0, column 1
- Bits 12-13: row 0, column 2
- Bits 14-15: row 0, column 3

(Serpentine: down on right half, up on left half within each 4-column block)

Verified hardware test results:
- Group 0, bit 0  -> row 3, col 4
- Group 0, bit 8  -> row 1, col 1
- Group 1, bit 0  -> row 7, col 4
- Group 1, bit 8  -> row 6, col 1

## Multi-Panel Chaining

When panels are chained, the data for the last panel in the chain must be
clocked in first (shift registers push data toward the output). The firmware
iterates panels in reverse order, then issues a single LAT pulse.

```
Clock panel[N-1] data, then panel[N-2], ..., then panel[0]
One LAT pulse latches all panels simultaneously
```

## OE (Output Enable)

OE is active LOW. The firmware pulls OE HIGH briefly during the LAT pulse
to prevent visual artifacts from partial frame data appearing on the panels,
then pulls OE LOW immediately after latching to enable output.
