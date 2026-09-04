# ESP32-C6 eInk CO2 meter

PlatformIO firmware for these Adafruit products:

- [ESP32-C6 Feather, product 5933](https://www.adafruit.com/product/5933)
- [2.13-inch 250x122 monochrome eInk breakout, product 4197](https://www.adafruit.com/product/4197)
- [SCD-40 CO2, temperature, and humidity sensor, product 5187](https://www.adafruit.com/product/5187)

At boot and after each wake, the firmware starts the SCD-40's low-power
periodic mode, reads one valid sample after about 30 seconds, stops measurement,
and renders the dashboard. The ESP32 then deep sleeps for one hour and repeats.
There is no five-minute sampling or averaging. The eInk image remains visible
while asleep; flashing during its several-second refresh is normal.

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
USB-JTAG, boot/NeoPixel, and LED pins; and leaves I2C/STEMMA QT available for
the SCD-40. GPIO 16 is reserved for SD chip select, so it is no longer available
as hardware UART TX.

## SCD-40 connection

The easiest connection is a STEMMA QT cable between the Feather and SCD-40. No
additional display pins are shared. For individual wires, use:

| SCD-40 | ESP32-C6 Feather | GPIO |
| --- | --- | ---: |
| `VIN` | `3V` | - |
| `GND` | `GND` | - |
| `SCL` | `SCL` | 18 |
| `SDA` | `SDA` | 19 |

The firmware displays temperature in Fahrenheit by default. Set
`kUseFahrenheit` to `false` in `src/main.cpp` to display Celsius.

## Deep sleep and interval tuning

Set `kSleepIntervalMinutes` near the top of `src/main.cpp`: `60` for one hour,
`120` for two hours. This is the sleep duration **after** each refresh, so the
interval between displayed readings also includes measurement and refresh time.
For a quick hardware check, temporarily set it to `1`.

The SCD-40 uses a start/read/stop sequence because it does not support true
single-shot CO2 measurement. Before host sleep, the firmware releases I2C and
its internal pull-ups, then drives GPIO20 (`PIN_NEOPIXEL_I2C_POWER`) low and
holds it there. This switches off the STEMMA QT supply, including the SCD-40
and its green power LED, without extra wiring. On wake, power is restored and
the firmware waits one second before initializing the sensor. This also powers
off any other devices on that STEMMA supply and the onboard NeoPixel.

This power switching requires the sensor to be powered through STEMMA QT;
a sensor wired directly to the Feather's `3V` pin stays powered.
See [Adafruit's low-power guide](https://learn.adafruit.com/adafruit-esp32-c6-feather/low-power-use).

A failed initialization or a measurement that produces no valid reading within
45 seconds displays a sensor error and sleeps before retrying next wake.

The display library sleeps the panel controller after refreshing. The firmware
holds `ENA` high during ESP32 deep sleep to keep the display/SRAM rail powered
and avoid back-powering it through control pins. It also retains SD deselection. This is ESP32 deep sleep, not a complete peripheral power
shutdown; total board current still needs to be measured on hardware.

## BOOT button programming mode

**Add a jumper from Feather `IO9` to `A3` (GPIO 5).** BOOT is connected to GPIO 9,
which cannot wake the ESP32-C6 from deep sleep. GPIO 5 is an unused wake-capable
input; the jumper lets the same button drive it. Both pins are configured as
inputs with pull-ups. Do not drive the onboard NeoPixel while using this jumper,
since it shares GPIO 9.

Press BOOT to wake immediately, and keep holding for ten seconds after firmware
starts to enter programming mode. The screen displays **PROGRAMMING MODE**, and
the firmware stays awake indefinitely for uploading. Release BOOT once that
message appears. Tap RESET with BOOT released to resume normal operation.
A shorter press wakes the meter for a normal reading and starts a new sleep
interval. Holding BOOT while already awake also enters programming mode.

Without the jumper, BOOT can enter programming mode only while the firmware is
already awake. To access it while sleeping, tap RESET first, then press and hold
BOOT once firmware starts. Holding BOOT *during reset* selects the ROM bootloader
instead; that remains an alternative recovery/upload path but cannot render the
programming-mode screen.

The ten-second hold is checked in firmware after wake, since the hardware wake
source triggers on a low level rather than timing the press. Allow additional
time for startup and the eInk refresh before the message appears. Programming
mode lasts until reset or power cycling; it is not saved in flash.

The microSD card shares `SCK`, `MOSI`, and `MISO` with the display and SRAM. Its
separate `SDCS` connection uses `TX` / GPIO 16. Until SD support is added, the
firmware holds this pin high to keep the card deselected. USB serial remains
available, but GPIO 16 should no longer be used as a hardware UART TX pin.

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

This particular display's ribbon is marked `FPC-7528B`, so the firmware uses
the `ThinkInk_213_Mono_BN` SSD1680 driver. Its framebuffer needs an 8-pixel
memory offset; using `ThinkInk_213_Mono_GDEY0213B74` causes a shifted image and
a noisy strip along one edge.

For a different panel, select its driver by its physical marking:

- `GDEY0213B74` / `FPC-A002`: use `ThinkInk_213_Mono_GDEY0213B74`.
- Very old SSD1675 product 4197 units: use `ThinkInk_213_Mono_B72`. The SSD1675
  and SSD1680 are not code-compatible.

Adafruit references:

- [eInk breakout pinout](https://learn.adafruit.com/adafruit-2-13-eink-display-breakouts-and-featherwings/pinouts)
- [ESP32-C6 Feather pinout](https://learn.adafruit.com/adafruit-esp32-c6-feather/pinouts)
- [Adafruit EPD Arduino library](https://github.com/adafruit/Adafruit_EPD)
- [Adafruit SCD-4x Arduino guide](https://learn.adafruit.com/adafruit-scd-40-and-scd-41/arduino)
- [Sensirion SCD4x Arduino library](https://github.com/Sensirion/arduino-i2c-scd4x)

## Hardware verification

- With a one-minute test interval, confirm one reading after about 30 seconds,
  followed by a sleep log, USB disconnect, timer wake, and a fresh reading.
  Confirm the SCD-40 green LED is off throughout sleep and lights again on wake.
- With IO9 jumpered to A3, hold BOOT during sleep until PROGRAMMING MODE appears.
  Release it, wait longer than the test interval, and confirm USB remains usable
  and the screen remains in programming mode. Upload, or RESET to resume.
- Try a short BOOT press during sleep: it should take a normal reading and sleep.
- Disconnect the sensor: confirm the error screen and a subsequent sleep/retry.
- Restore the intended interval before the final upload.
