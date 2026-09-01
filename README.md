# ESP32-C6 eInk smiley

PlatformIO firmware for these two Adafruit products:

- [ESP32-C6 Feather, product 5933](https://www.adafruit.com/product/5933)
- [2.13-inch 250x122 monochrome eInk breakout, product 4197](https://www.adafruit.com/product/4197)

On boot, the Feather draws one centered smiley and performs one full eInk
refresh. The display may flash during its several-second refresh; that is normal.
The finished image remains visible even after power is disconnected.

Solder the supplied header onto the eInk breakout before making these jumper
connections.

## Wiring

Use the breakout's header pins. The first column uses labels printed on the
Feather; the GPIO column is included to remove any ambiguity.

| ESP32-C6 Feather | GPIO | eInk breakout |
| --- | ---: | --- |
| `3V` | - | `VIN` / `3-5V` |
| `GND` | - | `GND` |
| `SCK` | 21 | `SCK` / `CLK` |
| `MO` / `MOSI` | 22 | `MOSI` |
| `MI` / `MISO` | 23 | `MISO` |
| `0` / `SS` | 0 | `ECS` |
| `A0` | 1 | `D/C` |
| `A5` | 2 | `SRCS` |
| `A4` | 3 | `RST` |
| `A2` | 6 | `ENA` |
| `7` | 7 | `BUSY` |
| `TX` | 16 | `SDCS` |

Leave the display's `3.3V out` pin disconnected. Be careful to use the
display's `VIN` input, not its `3.3V out` regulator output, for power.

This mapping uses the Feather's hardware SPI pins; avoids the C6 strapping,
USB-JTAG, boot/NeoPixel, and LED pins; and leaves the I2C/STEMMA QT and UART
pins available.

## Low-power operation

`ENA` controls the regulator that powers the display, SRAM, and microSD
circuitry. The firmware drives `A2` high before initializing the display, waits
for the full refresh to finish, and then drives it low. The eInk image remains
visible with this power rail off.

Cutting this power erases the breakout's SRAM buffer and removes power from the
microSD socket. After every wake, drive `ENA` high, wait briefly, call
`display.begin()`, redraw the complete frame, and initialize the SD card again.
Close all files before pulling `ENA` low, and never do so while
`display.display()` is still refreshing.

The microSD card shares `SCK`, `MOSI`, and `MISO` with the display and SRAM. Its
separate `SDCS` connection uses `TX` / GPIO 16. Until SD support is added, the
firmware holds this pin high to keep the card deselected. USB serial remains
available, but GPIO 16 should no longer be used as a hardware UART TX pin.

When MCU deep sleep is added, configure GPIO 6 to hold its low output level
during sleep; otherwise it may float and allow the breakout's enable pull-up to
turn the power rail back on. Release that hold after waking, before driving the
pin high again.

## Build and upload

From this directory:

```sh
pio run
pio run --target upload
pio device monitor
```

The serial monitor runs at 115200 baud. If PlatformIO cannot enter the ROM
bootloader automatically, hold **BOOT**, tap **RESET**, release **BOOT**, and run
the upload command again.

## Display revision

The firmware targets the `GDEY0213B74` SSD1680 panel that Adafruit has shipped
for product 4197 since August 30, 2024. This is important because it has a
different pixel-memory alignment from earlier revisions.

- For an SSD1680 unit shipped from May 9, 2021 through August 29, 2024, change
  the display type in `src/main.cpp` to `ThinkInk_213_Mono_BN`.
- For a very old SSD1675 unit shipped before May 9, 2021, use
  `ThinkInk_213_Mono_B72`. The SSD1675 and SSD1680 are not code-compatible.

Use the controller/panel marking when available; purchase date is only a guide.

Adafruit references:

- [eInk breakout pinout](https://learn.adafruit.com/adafruit-2-13-eink-display-breakouts-and-featherwings/pinouts)
- [ESP32-C6 Feather pinout](https://learn.adafruit.com/adafruit-esp32-c6-feather/pinouts)
- [Adafruit EPD Arduino library](https://github.com/adafruit/Adafruit_EPD)
