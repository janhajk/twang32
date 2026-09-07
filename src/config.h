/*
	TWANG32 - An ESP32 port of TWANG
	(c) B. Dring 3/2018
	License: Creative Commons 4.0 Attribution - Share Alike

	TWANG was originally created by Critters
	https://github.com/Critters/TWANG

	Basic hardware definitions

	Light Strip Notes:

	Noepixel / WS2812
		- Low Cost
		- You might already have a strip.
		- No Clock Line - This means a fixed and relatively slow data rate and only shorter strips can be used
		- Poor Dynamic Range - The low end of brightness is basically not visible
	Dotstar (Highly recommended)
	  - Higher Cost
		- Higher speed - Longer strips can be used.
		- Great dynamic range, so lower levels and more colors can be used.


*/

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Hardware: NodeMCU-32S (ESP32-WROOM-32)
//   GPIO 16 - LED data      (SK6812 / WS2812 strip)
//   GPIO 17 - LED clock     (only used by APA102/Dotstar strips)
//   GPIO 21 - MPU6050 SDA   (I2C default on this board)
//   GPIO 22 - MPU6050 SCL
//   GPIO 25 - DAC audio out (-> PAM8403 amplifier)
// ---------------------------------------------------------------------------
#define DATA_PIN 16
#define CLOCK_PIN 17

/* Game is rendered to this and scaled down to your strip.
 This allows level definitions to work on all strip lengths  */
#define VIRTUAL_LED_COUNT 1000

// What type of LED strip - uncomment exactly one of these.
//
// The two families are NOT interchangeable at runtime: single-wire strips carry
// their timing in the data signal, clocked strips have a separate clock line.
// Changing family means recompiling and rewiring.
//
//   USE_NEOPIXEL  single-wire: WS2812, WS2812B, SK6812 (incl. RGBW)
//                 data on DATA_PIN, CLOCK_PIN unused
//   USE_SK9822    clocked: SK9822, APA102, DOTSTAR
//                 data on DATA_PIN AND clock on CLOCK_PIN
//
// #define USE_NEOPIXEL
#define USE_SK9822

// Strip channel layout. This is a RUNTIME setting (stored in EEPROM, changeable
// over serial with "W=<n>" or via the WiFi UI) - the value below is only the
// default used on first boot or after a settings reset.
//
//   0 = RGB    3 bytes per pixel. Plain SK6812/WS2812 RGB strips.
//   1 = RGBW   4 bytes per pixel, white channel used. FastLED moves the common
//              part of R/G/B into W (kRGBWExactColors), which is more efficient
//              but tints "white" with whatever the W chip actually is - on a
//              warm-white (RGBWW) strip that looks yellowish.
//   2 = RGBW,  4 bytes per pixel, white channel always 0 (kRGBWNullWhitePixel).
//       W off  Correct byte count for a 4-channel strip, but colors stay
//              exactly as the game mixes them. Use this if mode 1 looks off on
//              a warm-white strip.
//
// Getting the channel count wrong makes the strip show garbage (every pixel
// shifted by one channel), so this is the first thing to check if the colors
// are scrambled.
//
// NOTE: this setting only applies to USE_NEOPIXEL. Clocked strips (USE_SK9822)
// are always 3-channel; the setting is accepted but ignored there.
#define STRIP_MODE_RGB 0
#define STRIP_MODE_RGBW 1
#define STRIP_MODE_RGBW_NO_WHITE 2

#define MIN_STRIP_MODE STRIP_MODE_RGB
#define MAX_STRIP_MODE STRIP_MODE_RGBW_NO_WHITE

#ifdef USE_NEOPIXEL
// SK6812 RGBWW strip -> default to the 4-channel layout.
#define DEFAULT_STRIP_MODE STRIP_MODE_RGBW
#else
// Clocked strips are RGB only.
#define DEFAULT_STRIP_MODE STRIP_MODE_RGB
#endif

// Check to make sure LED choice was done right
#if !defined(USE_NEOPIXEL) && !defined(USE_SK9822)
#error "You must have USE_SK9822 or USE_NEOPIXEL defined in config.h"
#endif

#if defined(USE_NEOPIXEL) && defined(USE_SK9822)
#error "Both USE_SK9822 and USE_NEOPIXEL are defined in config.h. Only one can be used"
#endif

// NOTE: All brightness values are 0.255 and will be scaled by the brightness set
// in FastLED as well (user_settings.led_brightness value)

#ifdef USE_SK9822
#define LED_TYPE SK9822					  // use SK9822HD for 5-bit gamma correction
#define LED_COLOR_ORDER BGR				  // usual order for this family; switch it if colors are wrong
// A clocked strip has real dynamic range at the bottom of the scale, so the
// dim effects can be genuinely dim instead of "lowest visible step".
#define CONVEYOR_BRIGHTNESS 8
#define LAVA_OFF_BRIGHTNESS 4
// Sized for the longest strip in use: 5 m at 144 LEDs/m is 720 pixels.
//
// Deliberately NOT VIRTUAL_LED_COUNT. FastLED's power estimator bills every
// registered pixel, including the ones that are not fitted, so an oversized
// value quietly eats the budget in POWER_LIMIT_MA. At 800 registered and 720
// fitted that phantom load is about 80 mA - raise this further only when a
// longer strip actually turns up.
//
// Timing is not the constraint here: a clocked strip runs at several MHz, so
// 720 pixels are roughly 2 ms per frame against a 16.7 ms budget.
#define MAX_LEDS 800
#define MIN_REDRAW_INTERVAL 1000.0 / 60.0 // divide by frames per second..if you tweak, adjust player speed
#endif

#ifdef USE_NEOPIXEL
#define LED_TYPE SK6812					  // SK6812; use WS2812B for the classic neopixel
#define LED_COLOR_ORDER GRB				  // SK6812/WS2812 are GRB, switch if the colors look wrong
#define CONVEYOR_BRIGHTNESS 40			  // low neopixel values are nearly off, Neopixels need a higher value
#define LAVA_OFF_BRIGHTNESS 15			  // low neopixel values are nearly off, Neopixels need a higher value
// A single-wire strip carries its timing in the data signal: 24 bits per pixel
// at 1.25 us each. That is a hard physical ceiling, not a setting -
//   300 pixels ~  9 ms per frame   (fits 60 fps)
//   500 pixels ~ 15 ms per frame   (only just)
//   720 pixels ~ 22 ms per frame   (60 fps impossible, ~45 fps at best)
// A long 144 LEDs/m strip therefore belongs on the clocked SK9822 branch. If
// it really has to be single-wire, raise this AND slow MIN_REDRAW_INTERVAL,
// and expect to retune the player speed with it.
#define MAX_LEDS 300
#define MIN_REDRAW_INTERVAL 1000.0 / 60.0 // divide by frames per second..if you tweak adjust player speed
#endif

// ---------------------------------------------------------------------------
// Power budget
//
// FastLED caps the brightness of every frame so the strip stays inside this
// budget. It matters here because tickBossKilled() raises the brightness to
// (led_brightness * 2) and lights the whole strip with a rainbow - that single
// effect is the current peak of the entire game (~2.4 A of LED draw at the
// default brightness of 100).
//
// The budget covers the LED STRIP ONLY. Subtract the rest of the electronics
// from what the PSU can deliver before setting it:
//   ESP32 with the WiFi AP running   ~250 mA average, 500 mA peak
//   PAM8403 amplifier                ~20 mA idle, up to 250 mA on transients
//   MPU6050                          ~4 mA
//   -> reserve 800 mA, and derate the PSU itself by 15%:
//      budget = psu_mA * 0.85 - 800
//
// Suggested values:
//   5 V / 3 A (USB-C)   -> 1800   (dims the boss-kill effect by ~25%)
//   5 V / 6 A           -> 4200   (never actually engages, pure safety net)
//   5 V / 10 A          -> 7500   (never actually engages)
//
// Powered from an ATX PC power supply: its +5V rail delivers 20 A and up, so
// the budget below is set for headroom rather than for the PSU's limit.
//
// Two caveats worth knowing when tuning this:
//   - FastLED's model has no idea the strip has a W channel. It bills the RGB
//     buffer at 16/11/15 mA and the driver only moves min(r,g,b) into W
//     afterwards, so on RGBW it OVERestimates by up to 3x. That errs on the
//     safe side: the limiter engages too early, never too late.
//   - addLeds() registers MAX_LEDS (300) pixels even though only 150 are
//     fitted. The 150 unused entries each bill 1 mA of "dark" current, so
//     there is another ~150 mA of phantom load in the calculation.
#define POWER_LIMIT_VOLTS 5
#define POWER_LIMIT_MA 4200

// Comment or remove the next #define to disable the /metrics endpoint on the HTTP server.
// This endpoint provides the Twang32 stats for ingestion via Prometheus.
#define ENABLE_PROMETHEUS_METRICS_ENDPOINT

#endif
