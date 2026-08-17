# T-Display-S3 ESPHome Component

ESPHome custom display component for the **LilyGo T-Display-S3** (ESP32-S3R8, ST7789V 170×320, 8-bit parallel I80 bus).

This is a fork of [landonr/lilygo-tdisplays3-esphome](https://github.com/landonr/lilygo-tdisplays3-esphome), rewritten to work with **ESPHome 2026.x and ESP-IDF 5.x**. The original Arduino/TFT_eSPI approach no longer compiles in that environment — see the background section below.

---

## Quick start

```yaml
external_components:
  - source: github://jesuli87/lilygo-tdisplays3-esphome@main
    components: [tdisplays3]

esphome:
  name: tdisplays3
  friendly_name: T-Display S3

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf
  flash_size: 16MB
  sdkconfig_options:
    CONFIG_SPIRAM_MODE_OCT: "y"
    CONFIG_SPIRAM_SPEED_80M: "y"

psram:
  mode: octal
  speed: 80MHz

output:
  - platform: ledc
    pin: GPIO38
    id: backlight_pwm
    frequency: 2000

light:
  - platform: monochromatic
    output: backlight_pwm
    name: "Backlight"
    restore_mode: RESTORE_DEFAULT_ON

font:
  - file: "gfonts://Roboto"
    id: roboto
    size: 30

display:
  - platform: tdisplays3
    id: disp
    update_interval: 5s
    rotation: 270
    lambda: |-
      it.fill(Color(0, 0, 0));
      it.printf(160, 70, id(roboto), Color(255, 255, 255), TextAlign::CENTER, "Hello!");
```

The component defaults to the correct physical dimensions (170×320). With `rotation: 270` the lambda coordinate space is 320×170 (landscape).

---

## Hardware

| Item | Value |
|---|---|
| SoC | ESP32-S3R8 (8 MB OPI PSRAM, 16 MB flash) |
| Display | ST7789V, 170×320 px |
| Bus | 8-bit parallel Intel 8080 (I80) |
| Backlight | GPIO38, LEDC PWM |
| Power enable | GPIO15 — must be driven HIGH before display init |

### Pin mapping

| Signal | GPIO |
|---|---|
| WR | 8 |
| RD | 9 |
| DC (data/command) | 7 |
| CS | 6 |
| RST | 5 |
| D0–D7 | 39, 40, 41, 42, 45, 46, 47, 48 |
| Backlight | 38 |
| VDD enable | 15 |

---

## Why not TFT_eSPI?

The original component bundled a patched TFT_eSPI and required `framework: arduino`. **ESPHome 2026.7+ uses the ESP-IDF 5.x toolchain natively — no Arduino runtime.**

TFT_eSPI's `Button.cpp` and `Smooth_font.cpp` reference `String`, `Arduino.h`, `int16_t`, and other Arduino types. Without the Arduino runtime these files do not compile. Patching individual files in TFT_eSPI is fragile and would break again whenever ESPHome updates ESP-IDF.

---

## Why GPIO39–48 cannot be driven by raw GPIO calls

Switching to ESP-IDF and writing directly to the data bus with `gpio_set_level()` produced a completely blank screen. A register diagnostic revealed the cause:

```
D0(GPIO39)=0  D7(GPIO48)=0   after gpio_set_level(pin, 1)
WR(GPIO8)=1                   works correctly
```

On ESP32-S3, the GPIO matrix output routing for GPIO32 and above does not function in this ESPHome + ESP-IDF 5.5.5 build. Calling `gpio_set_level()` on GPIO39–48 writes to an internal latch that is never forwarded to the pad.

The display worked on the original Arduino firmware because TFT_eSPI used the **ESP32-S3 LCD_CAM peripheral** (via the Arduino IDF I2S/LCD driver), which bypasses the GPIO matrix entirely and routes signals directly to the physical pads. Raw `gpio_set_level()` does not use LCD_CAM.

---

## Solution: LovyanGFX via LCD_CAM

[LovyanGFX](https://github.com/lovyan03/LovyanGFX) supports ESP-IDF natively and drives the 8-bit parallel bus through `Bus_Parallel8`, which internally uses the same LCD_CAM peripheral. No Arduino runtime required.

The key configuration differences from a generic ST7789:

- **`offset_x = 35`** — the ST7789V controller is 240 columns wide; the glass is only 170. Column 0 on the glass is at controller column 35.
- **`invert = true`** — the T-Display-S3 panel requires INVON for correct colors.
- **`freq_write = 16000000`** — matches the LilyGo factory firmware.
- **GPIO15 = HIGH before `lcd->init()`** — this pin switches the VDD rail for the display. If it stays low the panel never powers up.

The LGFX device class, bus, and panel configuration live in `components/tdisplays3/t_display_s3.cpp`. The ESPHome display buffer uses a full-screen LGFX sprite in OPI PSRAM; the lambda draws into the sprite, and `update()` blits it to the panel in one `pushSprite()` call.

---

## PSRAM and sdkconfig

The OPI PSRAM on the ESP32-S3R8 requires two non-default Kconfig settings. In ESPHome 2026.x these go under `sdkconfig_options`, **not** under `platformio_options` (those are ignored with the ESP-IDF toolchain):

```yaml
esp32:
  sdkconfig_options:
    CONFIG_SPIRAM_MODE_OCT: "y"
    CONFIG_SPIRAM_SPEED_80M: "y"
```

---

## First-time flashing

ESPHome OTA requires the device to already be running ESPHome. For a factory-fresh board:

1. Build the firmware: **Install → Manual Download → Modern format** in the ESPHome dashboard.
2. Put the board into flash mode: hold the left button while plugging in USB.
3. Open [web.esphome.io](https://web.esphome.io/) in Chrome or Edge, connect to the COM port, and install the `.bin`.
4. Subsequent updates can be done over Wi-Fi OTA.

---

## Contributions

- [@landonr](https://github.com/landonr) — original Arduino/TFT_eSPI component
- [@fisheradam](https://github.com/fisheradam) — documentation
- [@guillempages](https://github.com/guillempages) — external component structure, touch support
- [@bradmck](https://github.com/bradmck) — TFT_eSPI without patch
- [@jesuli87](https://github.com/jesuli87) — LovyanGFX/ESP-IDF rewrite for ESPHome 2026.x
