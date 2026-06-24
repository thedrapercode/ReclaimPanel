# Wiring Reference

## Panel Connector

The panels use a 20-pin ribbon cable (HUB16 style, NOT HUB75).

```
Pin 1  DR1 -> GPIO 25    Pin 2  DG1 -> GPIO 26    Pin 3  DB1 -> GPIO 27
Pin 4  DR2 -> GPIO 14    Pin 5  DG2 -> GPIO 33*   Pin 6  DB2 -> GPIO 13
Pin 7  DR3 -> GPIO 23    Pin 8  DG3 -> GPIO 19    Pin 9  DB3 -> GPIO 5
Pin 10 DR4 -> GPIO 17    Pin 11 DG4 -> GPIO 16    Pin 12 DB4 -> GPIO 4
Pin 13 DR5 -> GPIO 2     Pin 14 DG5 -> GPIO 15    Pin 15 DB5 -> GPIO 18
Pin 16 GND -> GND        Pin 17 CLK -> GPIO 22    Pin 18 GND -> GND
Pin 19 LAT -> GPIO 21    Pin 20 OE  -> GPIO 3
```

*DG2 remapped from GPIO 12 to GPIO 33. GPIO 12 is a strapping pin that causes
boot failures when driven HIGH at startup.

## Power

- ESP32: powered via USB (5V / ~500mA)
- Panels: dedicated 5V supply sized for your panel count
  - Each panel at full white: approximately 2-3A at 5V
  - 4 panels full white: fuse or supply for at least 12A at 5V
- **Shared ground is required.** Connect the 5V panel supply GND to the ESP32 GND pin.
  Without this, the panels will not work.

## Multi-Panel Chaining

Panels chain IN to OUT via ribbon cable:

```
ESP32 --> Panel 0 (IN) --> Panel 0 (OUT) --> Panel 1 (IN) --> Panel 1 (OUT) --> ... ETC....
```

- IN connector: left side of panel (when viewed from back)
- OUT connector: right side of panel (when viewed from back)
- All panels share the same 5V power supply rails

## Panel Physical Layout (2x2 example)

From the back:
```
[ Panel 0 ] [ Panel 1 ]
[ Panel 2 ] [ Panel 3 ]
```

From the front (mirrored):
```
[ Panel 1 ] [ Panel 0 ]
[ Panel 3 ] [ Panel 2 ]
```

Set the web interface for arrangement field to match your physical layout.
For the 2x2 above, the correct arrangement value is: `1,0,3,2`
