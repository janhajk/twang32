# TWANG32 — custom build

A one-dimensional LED-strip dungeon crawler on an ESP32, adapted for an
SK6812 RGBWW strip and documented against real hardware.

This is **not original work**. It is a small set of adaptations on top of an
existing project — see *Credits* below for the full chain, and *Changes vs.
upstream* for exactly what was touched.

## Credits

| | |
|---|---|
| Original concept | **Line Wobbler** by [Robin Baumgarten](https://robinbaumgarten.itch.io/line-wobbler) |
| Original game | **TWANG** by [Critters](https://github.com/Critters/TWANG) |
| ESP32 port | **TWANG32** by [bdring](https://github.com/bdring/TWANG32) ([Buildlog.net](http://www.buildlog.net/blog?s=twang)) |
| PlatformIO fork, extra levels, dual-MPU support | [foxblock/TWANG32-PlatformIO](https://github.com/foxblock/TWANG32-PlatformIO) by [Janek](https://janek.ing) |
| This repository | hardware adaptation + runtime strip-mode switch |

**Direct upstream:** <https://github.com/foxblock/TWANG32-PlatformIO>
**Base commit:** `9b5680b` (2025-09-20)

Enclosure models (STL/STEP), PCB gerbers and the bill of materials are **not**
duplicated here — get them from the upstream repository, which is where they
are maintained.

## License

Upstream is inconsistent about its license, and that inconsistency is carried
over rather than silently resolved:

- `LICENSE` in this repository is the original **MIT** license, © 2018 bdring.
- The source headers in `src/` state **Creative Commons 4.0 Attribution –
  Share Alike**.

Both permit redistribution with attribution. All original copyright and license
headers are left intact. If you reuse this, honour the stricter of the two
(CC BY-SA) unless you have clarified the situation with the upstream authors.

## Hardware

| Part | Choice |
|---|---|
| MCU | NodeMCU-32S (ESP32-WROOM-32) |
| LED strip | SK6812 **RGBWW**, 5 m @ 30 LEDs/m = **150 LEDs** |
| Sensor | 1x MPU6050 (address 0x68) |
| Audio | internal DAC (GPIO 25) -> PAM8403 amplifier -> speaker |
| Power | 5 V, external. See *Power* below. |

### Pinout

| GPIO | Function |
|---|---|
| 16 | LED data |
| 17 | LED clock (unused — SK6812 is single-wire) |
| 21 | MPU6050 SDA |
| 22 | MPU6050 SCL |
| 25 | DAC audio out (unamplified) |

GPIO 16/17 are free on an ESP32-WROOM-32. On a **WROVER** module they are taken
by the PSRAM and the LED pin has to move.

## Changes vs. upstream

### Bugfix: upstream HEAD does not compile

`screenSaverTick()` still had `case COLOR_WIPE` / `COLOR_WHEEL` / `COLOR_CIRCLE`
after those enum entries had been commented out. The cases are now commented out
to match.

### New: runtime-switchable strip mode (RGB / RGBW)

See the dedicated section below. Touches `config.h`, `TWANG32.ino`,
`settings.h` and `wifi_ap.h`.

### New: power budget

`POWER_LIMIT_MA` in `config.h` feeds `FastLED.setMaxPowerInVoltsAndMilliamps()`,
which caps per-frame brightness so the strip cannot exceed the supply. This
matters because `tickBossKilled()` raises brightness to `led_brightness * 2` and
lights the whole strip — the current peak of the entire game.

### Configuration

- `platformio.ini`: platform pinned to `espressif32@^6.9.0` (= arduino-esp32
  2.0.x). The sound engine uses the old `hw_timer` API
  (`timerBegin`/`timerAlarmWrite`) that arduino-esp32 3.x removed, so an
  unpinned platform would eventually break the build. Also adds the exception
  decoder to the monitor and a faster upload speed.
- `config.h`: `LED_TYPE` is `SK6812` (was the generic `NEOPIXEL`) with an
  explicit `LED_COLOR_ORDER GRB`; pinout documented at the top.
- `settings.h`: default LED count `NUM_LEDS` 30 -> 150. Only the *default* —
  the value lives in EEPROM and is changeable at runtime.

## Strip mode (RGB / RGBW)

The strip layout is a normal user setting stored in EEPROM, so swapping an RGBW
strip for an RGB one needs no re-flash. Serial command `W=<n>`, or the
"Strip mode" field in the WiFi UI.

| Value | Mode | Meaning |
|---|---|---|
| `0` | RGB | 3 bytes per pixel. Plain SK6812 / WS2812 RGB strips. |
| `1` | RGBW | 4 bytes per pixel, white channel in use. FastLED's `kRGBWExactColors` moves the common part of R/G/B into W — more efficient, but "white" takes on whatever the W chip actually is. On a warm-white strip that leans yellow. |
| `2` | RGBW, W off | 4 bytes per pixel, white channel always 0 (`kRGBWNullWhitePixel`). Correct byte count for a 4-channel strip, but colors stay exactly as the game mixes them. Use this if mode 1 looks off on a warm-white strip. |

Default on a fresh EEPROM: `1`.

**If the colors look like confetti, check this first.** A wrong channel count
shifts every pixel by one channel, which reads as random garbage rather than as
a color-order problem.

Implementation note: `CLEDController::setRgbw()` only stores a small struct that
the ESP32 driver reads on every `show()`, so the mode can be flipped at runtime
with no re-init. `applyStripMode()` caches the last applied value and clears the
strip on a change, since the byte count on the wire shifts.

`Rgbw`'s `white_color_temp` is accepted but ignored by every built-in FastLED
conversion function, so there is no color-temperature knob worth exposing.

## Power

Measured against this build, at the default brightness of 100/255:

| State | Current |
|---|---|
| Idle / menu | 0.4 A |
| Normal play | 0.65 A |
| Boss-kill effect (brightness doubled, full strip) | **2.9 A** |

A **5 V / 6 A** supply is comfortable; 5 A is enough. Note that an SK6812 RGBW
pixel driven through FastLED can never reach its 80 mA datasheet maximum:
`rgb_2_rgbw_exact()` *moves* `min(r,g,b)` into W rather than adding to it, so
the four output bytes never sum above 765. White is in fact the cheapest color
on RGBW (~20 mA instead of ~60 mA).

**Power injection at the far end of a 5 m strip is required, not optional.**
With feed at one end only, the ~0.6 Ω/m trace resistance drops around 3.6 V by
the far end during full-strip effects. Feeding both ends brings that to ~0.9 V.

Two caveats when tuning `POWER_LIMIT_MA`: FastLED's model has no idea the strip
has a W channel and overestimates RGBW draw by up to 3x (erring safe), and
`addLeds()` registers `MAX_LEDS` pixels regardless of how many are fitted, each
unused one billing 1 mA of "dark" current.

## Build / flash

```
pio run                 # compile
pio run -t upload       # flash over USB
pio device monitor      # serial console @ 115200
```

The NodeMCU-32S uses a CP2102 USB-UART bridge. On Windows no COM port appears
until the Silabs CP210x VCP driver is installed — get it from
<https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers> and
install it from an elevated shell:

```
pnputil /add-driver "<path>\silabser.inf" /install
```

### Note on EEPROM

`SETTINGS_VERSION` was raised 3 -> 4 for the strip-mode field. The first boot
after flashing therefore discards stored settings and reloads defaults —
expected, not a bug.

## Runtime config

WiFi AP `TWANG_AP`, password `12345678`, web UI at <http://192.168.4.1>. The
same settings are reachable over the serial console (`?` prints the menu).

Adjustable: LED count/offset, strip mode (`W`), brightness, screensaver
brightness, volume, joystick deadzone, attack threshold, lives per level.

The AP credentials are upstream's defaults and are deliberately unchanged so
existing documentation still applies. Change them in `wifi_ap.h` if that
matters to you.

## Verified on hardware

Checked on a real board rather than assumed:

- The strip is a genuine 4-channel RGBW. A static test pattern rendered
  correctly in strip modes 1 and 2 but not in mode 0 — which only happens when
  the strip really does want 4 bytes per pixel.
- `LED_COLOR_ORDER GRB` is right; red/green/blue blocks came out in order.
- **No level shifter needed.** A fully static pattern showed no flicker at all,
  so this strip accepts the ESP32's 3.3 V data level.
- The MPU6050 answers on 0x68 with `WHO_AM_I = 0x68`, i.e. a real MPU6050 and
  not an MPU6500/9250 clone (those report 0x70/0x71 and `twang_mpu.h` rejects
  them).
- Mode 1 was chosen over mode 2 on looks; it is also the cheaper one.
- **Audio works.** Upstream had never tested it ("NOT tested with audio, so
  audio might be buggy"); the square-wave DAC engine in `sound.h` runs
  unmodified. Verified with a standalone test that includes `sound.h` itself
  rather than reimplementing it, driving a PAM8403 (GF1002 board) into a 4 ohm
  speaker: distinct volume steps, clean 200-2000 Hz sweep.
- The amplifier's fixed ~24 dB gain means the board's volume pot sits near its
  minimum. Treat the pot as a one-time trim and control volume through `S=`,
  which also keeps it adjustable over WiFi. A 22-47 kOhm series resistor ahead
  of the pot would move the usable setting to mid-travel, but that only matters
  if the pot is meant to be a user-facing control.

## Open points

- Audio volume is effectively a software-only control. See the note under
  *Verified on hardware*.
- The joystick still needs calibrating once the MPU6050 is mounted on the
  spring: axis (`JOYSTICK_ORIENTATION`), direction, deadzone and attack
  threshold. Build with `-DJOYSTICK_DEBUG` to see raw values.
- Play statistics are written to EEPROM on every game-over. An unattended
  device that keeps dying accumulates flash writes; worth batching.
- `SoundData.h` (14 kB of WAV data) is dead code — it is not included anywhere.
  It would become useful if the square-wave engine were replaced by sampled
  audio over I2S, which is upstream's stated intention.
