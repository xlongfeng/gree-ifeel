# GREE iFeel — Design Document

## Overview

**gree-ifeel** is an ESP32-C3 firmware that implements an autonomous room-temperature
controller for a GREE split air conditioner. It reads the ambient temperature from a
DS18B20 sensor, adjusts the AC setpoint via IR commands, and shows status on a
72×40-pixel SSD1315 OLED display.

---

## Hardware

| Component | Part | Connection |
|---|---|---|
| MCU | ESP32-C3 | — |
| Temperature sensor | DS18B20 (1-Wire) | GPIO 7 (default) |
| IR transmitter | 38 kHz IR LED | GPIO 0 (default) |
| Display | SSD1315 72×40 OLED (I2C) | SDA=GPIO5, SCL=GPIO6 |
| F1 button | Momentary push-button | GPIO 3 (default), pull-up |
| F2 button | Momentary push-button | GPIO 4 (default), pull-up |
| F3 button | Momentary push-button | GPIO 10 (default), pull-up |

> All GPIO numbers are configurable via `idf.py menuconfig` → **iFeel GPIO Configuration**.

### SSD1315 notes

The SSD1315 is command-compatible with SSD1306 but requires:
- **COM pins**: `SET_COMPINS (0xDA) = 0x12` (alternating) — the ESP-IDF driver sends
  `0x02` (sequential) for height ≠ 64, which causes 2× height stretching. Overridden
  in firmware after `esp_lcd_panel_init()`.
- **Brightness**: the display must be configured while OFF. Three registers are set
  together for a visible effect:
  - `0x81 = 0x20` — contrast
  - `0xD9 = 0x11` — pre-charge period
  - `0xDB = 0x00` — VCOMH deselect

---

## Modules

### `main.c` — Entry point
Initialises all subsystems in order: console → UI → IR → iFeel → thermometer → buttons.

### `ifeel.c` — State machine
Core control logic.

**States:**

```
  ┌─────────────────────────────────────────────────────┐
  │  IFEEL_OFF                                          │
  │  • AC off                                           │
  │  • Bar blinks at 1s                                 │
  │  • Top label: "GREE iFeel"                          │
  │  • No temperature monitoring                        │
  └──────────────┬──────────────────────────────────────┘
                 │ F1 button (short press)
  ┌──────────────▼──────────────────────────────────────┐
  │  IFEEL_ON                                           │
  │  • AC on (COOL mode, setpoint=27°C default)         │
  │  • Bar fills over 5-minute monitor window           │
  │  • Top label: "ST: xx°C"                            │
  │  • Monitor fires every 5 min → auto-adjust setpoint │
  └─────────────────────────────────────────────────────┘
```

**Temperature control (ON state only):**

| Room temperature | Action |
|---|---|
| > 25.6°C | Decrease setpoint by 1°C (min 24°C) |
| < 24.0°C | Increase setpoint by 1°C (max 28°C) |
| 24.0–25.6°C | No change |

Monitor interval: **300 seconds (5 minutes)**.

**Buttons:**

| Button | Short press | Long press |
|---|---|---|
| F1 | Toggle OFF ↔ ON | *(unused)* |
| F2 | Increment setpoint (24→25→…→28→24, ON only); cycle limit index when limit window visible | Show/hide limit config window |
| F3 | Toggle AC display light (any state) | *(unused)* |

### `thermometer.c` — DS18B20 reader
Spawns a FreeRTOS task that reads temperature every second via 1-Wire/RMT and calls
`ifeel_on_temperature(float)`.

### `gree_ir.c` — GREE IR transmitter
Encodes a `gree_ac_state_t` into the GREE protocol (2 × 8-byte frames, 38 kHz carrier)
and transmits via RMT. Full state including mode, fan, swing, turbo, sleep, light, etc.

### `button.c` — GPIO button driver
Interrupt-driven with FreeRTOS task debounce. Events are dispatched to the
active window handler via `button_set_dispatch()`.

- Falling edge → queue event
- Debounce: 50 ms
- **Short press**: button released before 1000 ms → dispatch with `long_press=false`
- **Long press**: button still held at 1000 ms → dispatch with `long_press=true` immediately,
  polling exits, waits for physical release before re-arming

### `ui.c` — Display driver + LVGL UI

#### Display pipeline

```
LVGL (I1 mono) → lvgl_flush_cb → oled_buffer (page format) → SSD1315 via I2C
```

The flush callback converts LVGL's I1 bitmap (1 bit/pixel, row-major) to the
SSD1315's page format (8 rows packed vertically per byte, column-major). Color
inversion: LVGL black → OLED bright pixel; LVGL white → OLED off pixel.

#### Screen layout (72 × 40 px)

Two full-screen windows stacked; z-order switches on state change. A third limit config window can be overlaid on either.

**Main window** (foreground when OFF):
```
┌──────────────────────────────────────────────┐
│  Gree iFeel                                  │
│  RT: xx.x°C                                  │
└──────────────────────────────────────────────┘
```

**Monitor window** (foreground when ON):
```
┌──────────────────────────────────────────────┐
│  ST: xx.0°C                                  │
│  RT: xx.x°C                                  │
│  ████████░░░░░░░░░░  (progress bar)           │
└──────────────────────────────────────────────┘
```

**Limit config window** (brought to front on temp long press; auto-hides after 8 s):
```
┌──────────────────────────────────────────────┐
│  HT: xx.x°C                                  │
│  LT: xx.x°C                                  │
└──────────────────────────────────────────────┘
```

Limit range: LOW 23.4–24.4°C, HIGH 25.0–26.0°C, 6 steps × 0.2°C stride, default index 3 ([24.0, 25.6]°C).

#### LVGL configuration

- Color format: `LV_COLOR_FORMAT_I1`
- Theme: `lv_theme_mono` (`dark_bg=false`)
- Draw buffer: `72×40/8 + 8 = 368` bytes (full-screen, single buffer)
- LVGL tick: 5 ms periodic esp_timer
- LVGL task: priority 2, guarded by `_lock_t lvgl_api_lock`

#### Public API (`ui.h`)

```c
esp_err_t ui_init(void);
void ui_show_monitor(bool show);               // switch window z-order
void ui_set_st(const char *text);             // monitor: setpoint label
void ui_set_rt(const char *text);             // both windows: RT label
void ui_set_bar(int value, int min, int max); // monitor: progress bar
void ui_show_limit(bool show);                // overlay limit config window
void ui_set_ht(const char *text);             // limit: high-temp label
void ui_set_lt(const char *text);             // limit: low-temp label
```

---

## Build configuration (`sdkconfig.defaults`)

```
CONFIG_LV_CONF_SKIP=y
CONFIG_LV_USE_OBSERVER=y
CONFIG_LV_USE_SYSMON=y
CONFIG_LV_FONT_MONTSERRAT_12=y
CONFIG_LV_USE_THEME_MONO=y
```

---

## Module dependency diagram

```
main.c
  ├── console.c     (USB Serial JTAG RX drain)
  ├── ui.c          (SSD1315 OLED via I2C + LVGL)
  ├── gree_ir.c     (IR TX via RMT)
  ├── ifeel.c       (state machine)
  │     ├── ui.c   (label / bar updates)
  │     └── gree_ir.c (send AC commands)
  ├── thermometer.c (DS18B20 via 1-Wire/RMT)
  │     └── ifeel.c (on_temperature callback)
  └── button.c      (GPIO interrupt + debounce)
        └── ifeel.c (button callbacks)
```
