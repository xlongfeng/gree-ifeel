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
After editing `sdkconfig.defaults`, delete `sdkconfig` and run `idf.py set-target esp32c3` before building.

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
                │        └─► ui.c       (update labels / bar / windows)
```

`ifeel.c` is the single owner of AC state. It calls into every other module;
no other module calls back into `ifeel.c` (except `thermometer.c` via callback).

`ui.c` owns the LVGL task and the display lock. All public `ui_*` functions
acquire the recursive mutex internally — callers must **not** hold the lock.

## Key conventions

### LVGL color inversion
The SSD1315 flush callback inverts I1 colors:
- LVGL **black** → OLED **bright** (pixel ON)
- LVGL **white** → OLED **dark** (pixel OFF)

Always use `lv_color_black()` for anything that should be visible on screen.

### LVGL locking
`ui.c` uses a `SemaphoreHandle_t` recursive mutex (`s_lvgl_mutex`). It is exposed
via `ui_lock()` / `ui_unlock()` for callers that need to make multiple LVGL calls
atomically. All `ui_set_*` and `ui_show_*` functions acquire it internally.
Do not call LVGL APIs directly from outside `ui.c`.

### LVGL timer vs ESP timer
All auto-hide timers (`msg_timer`, `limit_timer`) use `lv_timer_t` (runs in LVGL
task). Do **not** use `esp_timer` for anything that calls LVGL APIs — those
callbacks run in a separate task.

### Window system (ifeel.c)
Four LVGL windows stacked on a single screen:
- **main** — shown when OFF
- **monitor** — shown when ON
- **limit** — overlaid on either; auto-hides after 8 s; any key except `LV_KEY_BUTTON_1` dismisses it
- **msg** — overlaid on everything; auto-hides after 500 ms; all keys ignored

Each window has an invisible `lv_obj_t` controller added to its LVGL group.
`ifeel.c` uses `group_push(g)` / `group_pop()` → `lv_indev_set_group()` to
route button events to the active window. Guard booleans (`s_*_pushed`) prevent
double push/pop.

### Button driver (`button.c`)
Uses `LV_INDEV_TYPE_KEYPAD`, polled every 20 ms. Each GPIO button is registered
with `button_init(gpio, key, hold)`.

Two logical key codes per physical button (defined in `button.h`):
- `LV_KEY_BUTTON_x` — click (short press, 0x1000/0x1002/0x1004)
- `LV_KEY_ALT_BUTTON_x` — hold (long press, 0x1001/0x1003/0x1005)

All codes are above 0x100 to avoid collision with LVGL reserved keys (0x00–0x7F;
notably `LV_KEY_ENTER = 10` collides with GPIO10).

**Event semantics** (pending queue stores `{key, lv_indev_state_t}` pairs):
- **Short press**: after physical release → `{BUTTON_x, PRESSED}` + `{BUTTON_x, RELEASED}`
- **Long press**: at 1000 ms → `{ALT_BUTTON_x, PRESSED}`; at physical release → `{ALT_BUTTON_x, RELEASED}`

`ifeel.c` handlers receive `LV_EVENT_KEY` (fires on PRESSED transition only for
non-ENTER keys). `LV_EVENT_LONG_PRESSED` / `CLICKED` are not used.

### SSD1315 hardware quirks
Two overrides are applied after `esp_lcd_panel_init()` in `ui_init()`:
1. `SET_COMPINS (0xDA) = 0x12` — alternating COM pins (ESP-IDF driver sends `0x02`
   for height ≠ 64, causing 2× height stretch).
2. Brightness registers `0x81`, `0xD9`, `0xDB` — must be sent while display is **OFF**
   (before `esp_lcd_panel_disp_on_off()`); sending them after has no effect on SSD1315.

### sdkconfig.defaults
`sdkconfig` is gitignored. Any LVGL or IDF option that must be set persistently
goes in `sdkconfig.defaults`. Currently includes: LVGL mono theme, Montserrat 12
and 24 fonts, observer/sysmon, and GPIO pin defaults.

### Fonts
- All labels except the msg window: `lv_font_montserrat_12`
- Msg window icon label: `lv_font_montserrat_24`

Both fonts must be enabled in `sdkconfig.defaults`. Do not introduce other font sizes.

### Temperature control (ifeel.c)
Setpoint range: 24–28°C, default 24°C. Monitor fires every 300 s in ON state.
Limit table: 6 steps × 0.2°C stride.
- Index 0: LOW = 23.4°C, HIGH = 25.0°C
- Index 5: LOW = 24.4°C, HIGH = 26.0°C

### Formatting
`.clang-format` is present: LLVM style, 4-space indent, 120-column limit,
K&R-style braces except functions get their own line. Run `clang-format -i` on
changed C/H files before committing.

`.cmake-format.py` is present: 4-space indent, 80-column limit. Run
`cmake-format -i` on any changed `CMakeLists.txt` or `.cmake` files before
committing.

### Commits
Never create a git commit unless the user explicitly asks. Complete the implementation
and verify the build first, then wait for the user to say "commit" (or similar)
before running `git commit`.
