# RGB Matrix Hardware Baseline

## Confirmed host connection

- USB bridge: CH340
- Windows serial port: COM20
- Board family: 30-pin ESP32 DevKit-style board (HW-394 pinout)
- Target for future ESP-IDF projects: `esp32` (classic ESP32), not `esp32s3`

The board image confirms a classic ESP32 development board layout. The exact
silicon/package and flash size still need an `esptool chip-id` / `flash-id`
read while the BOOT button is held during reset.

## HUB75 signal mapping

Use this mapping for a single 64x64, 1/32-scan Waveshare RGB matrix.
It intentionally avoids the common ESP32 boot-strap pins GPIO0, GPIO2,
GPIO4, GPIO5, GPIO12, and GPIO15.没有可提供视图数据的已注册数据提供程序。

| HUB75 signal | ESP32 GPIO |
| --- | ---: |
| R1 | 25 |
| G1 | 26 |
| B1 | 27 |
| R2 | 14 |
| G2 | 18 |
| B2 | 13 |
| A | 23 |
| B | 22 |
| C | 19 |
| D | 17 |
| E | 32 |
| LAT / STB | 21 |
| OE | 33 |
| CLK | 16 |
| GND | ESP32 GND |

The connector labels are HUB75 signal names. Every right-hand value is the
ESP32 GPIO number used in the C code. Labels such as `MTMS` are alternate JTAG
functions printed on some boards; they do not create a separate pin number.

For the initial test, the firmware scans the 1/32 panel continuously and
shows solid red, green, and blue for one second each. This is expected to be a
full-screen color cycle, not a static one-second GPIO output.

Do not use GPIO34 through GPIO39: they are input-only. Do not connect HUB75
power to the ESP32 board.

## Power and signal requirements

- Power the matrix from a separate, regulated 5 V supply rated for at least
  3 A. Keep the initial brightness low in firmware.
- Connect the matrix power-supply GND to ESP32 GND.
- Use a 3.3 V-to-5 V `74AHCT245` or `74HCT245` level shifter for the 14 HUB75
  signal lines. Direct 3.3 V wiring sometimes works, but is not a reliable
  design target for a 64x64 panel.
- Connect the ribbon cable to the panel `IN` connector, not `OUT`.

## Software baseline

The downloaded `ESP32` directory contains Arduino C++ demos. The future
implementation in this directory will be a native ESP-IDF C project targeting
`esp32`. The Waveshare ESP-IDF web demo targets an ESP32-S3 board and its pin
configuration must not be reused on this board.
