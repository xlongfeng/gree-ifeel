# Copilot Instructions — gree-ifeel

ESP32-C3 firmware (ESP-IDF) that controls a GREE split AC via IR based on DS18B20
room-temperature readings, with status shown on a 72×40 SSD1315 OLED.

## Build

Requires ESP-IDF to be sourced first:

```bash
. $IDF_PATH/export.sh       # or use esp-idf/export.sh from the local install
idf.py build                # full build
idf.py flash                # flash to device
idf.py monitor              # serial console
idf.py flash monitor        # flash + open monitor in one step
```

`sdkconfig` is gitignored. Persistent config lives in `sdkconfig.defaults`.
After editing `sdkconfig.defaults`, delete `sdkconfig` and rebuild to apply.

To adjust GPIO pins or other settings: `idf.py menuconfig` → **iFeel GPIO Configuration**.

There are no unit tests.

## Architecture

All source is in `main/`. The dataflow is:

```
DS18B20 (1s tick)
    └─► thermometer.c ──► ifeel_on_temperature()
                               │
                               ▼
button.c ──► ifeel.c (state machine: OFF / ON)
                │        │
                │        ├─► gree_ir.c  (send IR command via RMT)
                │        ├─► led.c      (brief status flash)
                │        └─► ui.c       (update labels / bar)
                │
                └─► ui.c (set_top_label / set_mid_label / set_bar_blinking / set_bar)
```

`ifeel.c` is the single owner of AC state. It calls into every other module;
no other module calls back into `ifeel.c` (except `thermometer.c` via callback).

`ui.c` owns the LVGL task and the display lock. All public `ui_*` functions
acquire `lvgl_api_lock` internally — callers must **not** hold the lock.

## Key conventions

### LVGL color inversion
The SSD1315 flush callback inverts I1 colors:
- LVGL **black** → OLED **bright** (pixel ON)
- LVGL **white** → OLED **dark** (pixel OFF)

Always use `lv_color_black()` for anything that should be visible on screen.

### LVGL locking
`ui.c` exposes no lock functions externally. Every `ui_set_*` function acquires
`_lock_t lvgl_api_lock` internally. Do not call LVGL APIs from outside `ui.c`.

### SSD1315 hardware quirks
Two overrides are applied after `esp_lcd_panel_init()` in `ui_init()`:
1. `SET_COMPINS (0xDA) = 0x12` — alternating COM pins (ESP-IDF driver sends `0x02`
   for height ≠ 64, causing 2× height stretch).
2. Brightness registers `0x81`, `0xD9`, `0xDB` — must be sent while display is **OFF**
   (before `esp_lcd_panel_disp_on_off()`); sending them after has no effect on SSD1315.

### sdkconfig.defaults
`sdkconfig` is gitignored. Any LVGL or IDF option that must be set persistently
goes in `sdkconfig.defaults`. Currently: LVGL mono theme, Montserrat 12 font,
observer/sysmon, and GPIO pin defaults.

### Formatting
`.clang-format` is present: LLVM style, 4-space indent, 120-column limit,
K&R-style braces except functions get their own line. Run `clang-format -i` on
changed files before committing.

### Button callbacks
`button_init(gpio, on_short_press, on_long_press)` — either callback may be NULL.
Short press fires on release (<1000 ms); long press fires immediately at 1000 ms
and stops polling. Existing buttons only use short press.

### Temperature thresholds (ifeel.c)
`IFEEL_TEMP_HIGH = 25.5°C` / `IFEEL_TEMP_LOW = 23.8°C`.
Setpoint range: 24–28°C. Monitor fires every 300 s (5 min) in ON state only.

### Font
All labels use `lv_font_montserrat_12`. Do not introduce other font sizes.

### Commits
Never create a git commit unless the user explicitly asks. Complete the implementation
and verify the build first, then wait for the user to say "create a commit" (or similar)
before running `git commit`.
