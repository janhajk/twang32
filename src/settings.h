#ifndef SETTINGS_H
#define SETTINGS_H

#include <EEPROM.h>
#include "sound.h"
#include "config.h"

// Version 2 adds the number of LEDs

// change this whenever the saved settings are not compatible with a change
// (i.e. when you add, remove or reorder members of the settings_t struct)
// It forces a reset from defaults.
#define SETTINGS_VERSION 4

// LEDS
#define NUM_LEDS 150 // 5m strip @ 30 LEDs/m (changeable at runtime via serial/WiFi)
#define MIN_LEDS 30

// for WS2812 sensible values are >50 and <200 (above brightness will only
// minimally increase, but current drawn increases a lot)
#define DEFAULT_BRIGHTNESS 100
#define DEFAULT_BRIGHTNESS_SCREENSAVER 50
#define MIN_BRIGHTNESS 5
#define MAX_BRIGHTNESS 255

// PLAYER
const uint8_t MAX_PLAYER_SPEED = 10; // Max move speed of the player
const uint8_t LIVES_PER_LEVEL = 3;	 // default lives per level
#define MIN_LIVES_PER_LEVEL 1
#define MAX_LIVES_PER_LEVEL 9

// JOYSTICK
#define JOYSTICK_ORIENTATION 1		   // 0, 1 or 2 to set the axis of the joystick
#define JOYSTICK_DIRECTION 1		   // 0/1 to flip joystick direction
#define DEFAULT_ATTACK_THRESHOLD 30000 // The threshold that triggers an attack
#define MIN_ATTACK_THRESHOLD 20000
#define MAX_ATTACK_THRESHOLD 30000

#define DEFAULT_JOYSTICK_DEADZONE 8 // Angle to ignore
#define MIN_JOYSTICK_DEADZONE 3
#define MAX_JOYSTICK_DEADZONE 12

// AUDIO
#define DEFAULT_VOLUME 20 // 0 to 255
#define MIN_VOLUME 0
#define MAX_VOLUME 255

long lastInputTime = 0;

// TODO ... move all the settings to this file.

#define LED_LENGTH (user_settings.led_end - user_settings.led_offset)
#define FOREACH_LED(iter) for (int iter = user_settings.led_offset; iter < user_settings.led_end; iter++)

typedef struct
{
	uint8_t settings_version; // stores the settings format version

	uint16_t led_offset;
	uint16_t led_end;
	uint8_t led_brightness;
	uint8_t led_brightnessScreensaver;
	uint8_t strip_mode; // STRIP_MODE_* from config.h - RGB vs RGBW wiring

	uint8_t joystick_deadzone;
	uint16_t attack_threshold;

	uint8_t audio_volume;

	uint8_t lives_per_level;

	// saved statistics
	uint16_t games_played;
	uint32_t total_points;
	uint16_t high_score;
	uint16_t boss_kills;

} settings_t;

typedef struct settings_param_t
{
	char code;
	bool hasValue;
	uint16_t newValue;
} settings_param_t;

// Function prototypes
// void reset_settings();
void settings_init();
void show_game_stats();
void settings_eeprom_write();
void settings_eeprom_read();
settings_param_t settings_processSerial(char inChar);
settings_param_t settings_fromString(char *line, int len);
void settings_set(char code, bool hasValue, uint16_t newValue);
void show_settings_menu();
void reset_settings();

const settings_param_t SET_PARAM_INVALID = {0};

settings_t user_settings;
int levelNumber = 0;
int score = 0;

#define READ_BUFFER_LEN 10
char readBuffer[READ_BUFFER_LEN];
uint8_t readIndex = 0;

void settings_init()
{
	settings_eeprom_read();
	show_settings_menu();
	show_game_stats();
}

settings_param_t settings_processSerial(char inChar)
{
	static char readBuffer[READ_BUFFER_LEN];
	static unsigned int readIndex = 0;

	assert(readIndex < READ_BUFFER_LEN);

	settings_param_t param = SET_PARAM_INVALID;

	switch (inChar)
	{
	case '\r': // ignore carriage return
		break;

	case '\b':
		if (readIndex > 0) readIndex--;
		break;

	case '\n': // parse as settings
		readBuffer[readIndex] = 0;
		if (readIndex > 0)
			param = settings_fromString(readBuffer, readIndex);
		readIndex = 0;
		break;

	default:
		if (readIndex == READ_BUFFER_LEN - 1) // leave room for 0 terminator
			readIndex = 0;					  // too many characters. Reset and try again
		else
			readBuffer[readIndex++] = inChar;
	}

	return param;
}

settings_param_t settings_fromString(char *line, int len)
{
	assert(line);
	assert(len > 0);
	assert(len < READ_BUFFER_LEN);

	if (len == 1)
	{
		return {.code = line[0], .hasValue = false};
	}

	if (len < 3)
	{
		Serial.printf("ERROR: Malformed serial command: %s\r\n", line);
		Serial.println("Valid commands need to be in the form X or X=nn. Enter ? for help.");
		return SET_PARAM_INVALID;
	}

	if (line[1] != '=')
	{
		Serial.printf("ERROR: Malformed serial command: %s\r\n", line);
		Serial.println("Valid commands need to be in the form X or X=nn. Enter ? for help.");
		return SET_PARAM_INVALID;
	}

	for (int idx = 2; idx < len; ++idx)
	{
		if (!isdigit(line[idx]))
		{
			Serial.printf("ERROR: Malformed value in serial command: %s\r\n", line);
			return SET_PARAM_INVALID;
		}
	}

	uint16_t val = atoi(line + 2);
	return {.code = line[0], .hasValue = true, .newValue = val};
}

const char *strip_mode_name(uint8_t mode)
{
	switch (mode)
	{
	case STRIP_MODE_RGB: return "RGB";
	case STRIP_MODE_RGBW: return "RGBW";
	case STRIP_MODE_RGBW_NO_WHITE: return "RGBW, W channel off";
	default: return "unknown";
	}
}

bool settings_param_valid(settings_param_t param)
{
	return param.code != 0;
}

void settings_set(settings_param_t param)
{
	if (!settings_param_valid(param))
		return;

	lastInputTime = millis(); // reset screensaver count

	if (param.hasValue)
	{
		switch (param.code)
		{
			case 'E': // LED count
				user_settings.led_end = constrain(param.newValue, MIN_LEDS, MAX_LEDS);
				if (user_settings.led_offset > user_settings.led_end-MIN_LEDS)
					user_settings.led_offset = user_settings.led_end-MIN_LEDS;					
				settings_eeprom_write();
				Serial.printf("Set LED count to %d\r\n", user_settings.led_end);
				break;
			case 'W': // strip mode (RGB / RGBW)
				user_settings.strip_mode = constrain(param.newValue, MIN_STRIP_MODE, MAX_STRIP_MODE);
				settings_eeprom_write();
				Serial.printf("Set strip mode to %d (%s)\r\n", user_settings.strip_mode,
					strip_mode_name(user_settings.strip_mode));
				break;
			case 'O': // LED offset
				user_settings.led_offset = constrain(param.newValue, 0, user_settings.led_end-MIN_LEDS);
				settings_eeprom_write();
				Serial.printf("Set LED offset to %d\r\n", user_settings.led_offset);
				break;
			case 'B': // brightness
				user_settings.led_brightness = constrain(param.newValue, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
				settings_eeprom_write();
				Serial.printf("Set brightness to %d\r\n", user_settings.led_brightness);
				break;
			case 'C': // screensaver brightness
				user_settings.led_brightnessScreensaver = constrain(param.newValue, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
				settings_eeprom_write();
				Serial.printf("Set screensaver brightness to %d\r\n", user_settings.led_brightnessScreensaver);
				break;
			case 'S': // sound
				user_settings.audio_volume = constrain(param.newValue, MIN_VOLUME, MAX_VOLUME);
				settings_eeprom_write();
				Serial.printf("Set audio volume to %d\r\n", user_settings.audio_volume);
				break;
			case 'D': // deadzone, joystick
				user_settings.joystick_deadzone = constrain(param.newValue, MIN_JOYSTICK_DEADZONE, MAX_JOYSTICK_DEADZONE);
				settings_eeprom_write();
				Serial.printf("Set deadzone to %d\r\n", user_settings.joystick_deadzone);
				break;
			case 'A': // attack threshold, joystick
				user_settings.attack_threshold = constrain(param.newValue, MIN_ATTACK_THRESHOLD, MAX_ATTACK_THRESHOLD);
				settings_eeprom_write();
				Serial.printf("Set attack threshold to %d\r\n", user_settings.attack_threshold);
				break;
			case 'L': // lives per level
				user_settings.lives_per_level = constrain(param.newValue, MIN_LIVES_PER_LEVEL, MAX_LIVES_PER_LEVEL);
				settings_eeprom_write();
				Serial.printf("Set lives to %d\r\n", user_settings.lives_per_level);
				break;
			case 'V': // skip to level
				levelNumber = param.newValue;
				Serial.printf("Skipping to level %d...\r\n", param.newValue);
				break;
			default:
				Serial.printf("ERROR: Unknown setting %c=%d\r\n", param.code, param.newValue);
				return;
		}
	}
	else
	{
		switch (param.code)
		{
		case '?': // show stats and settings
			show_game_stats();
			show_settings_menu();
			break;
		case 'R': // reset everything
			reset_settings();
			settings_eeprom_write();
			show_settings_menu();
			break;
		case 'P': // reset stats only
			user_settings.games_played = 0;
			user_settings.total_points = 0;
			user_settings.high_score = 0;
			user_settings.boss_kills = 0;
			settings_eeprom_write();
			break;
		case '!': // restart ESP
			ESP.restart();
			break;
		default:
			Serial.printf("ERROR: Unknown setting: %c\r\n", param.code);
		}
	}

}

void reset_settings()
{
	user_settings.settings_version = SETTINGS_VERSION;

	user_settings.led_offset = 0;
	user_settings.led_end = NUM_LEDS;
	user_settings.led_brightness = DEFAULT_BRIGHTNESS;
	user_settings.led_brightnessScreensaver = DEFAULT_BRIGHTNESS_SCREENSAVER;
	user_settings.strip_mode = DEFAULT_STRIP_MODE;

	user_settings.joystick_deadzone = DEFAULT_JOYSTICK_DEADZONE;
	user_settings.attack_threshold = DEFAULT_ATTACK_THRESHOLD;

	user_settings.audio_volume = DEFAULT_VOLUME;

	user_settings.lives_per_level = LIVES_PER_LEVEL;

	user_settings.games_played = 0;
	user_settings.total_points = 0;
	user_settings.high_score = 0;
	user_settings.boss_kills = 0;

	Serial.println("Settings reset...");

	settings_eeprom_write();
}

void show_settings_menu()
{
	Serial.println();
	Serial.println("====== TWANG Settings Menu ========");
	Serial.println("=    Current values are shown     =");
	Serial.println("=   Send new values like B=150    =");
	Serial.println("=          with a newline         =");
	Serial.println("===================================");

	Serial.println();
	Serial.printf("O=%d (LED Offset %d-%d)\r\n", user_settings.led_offset, 0, user_settings.led_end-MIN_LEDS);
	Serial.printf("E=%d (LED End/Count %d-%d)\r\n", user_settings.led_end, MIN_LEDS, MAX_LEDS);
	Serial.printf("B=%d (LED Brightness %d-%d)\r\n", user_settings.led_brightness, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
	Serial.printf("C=%d (Screensaver Brightness %d-%d)\r\n", user_settings.led_brightnessScreensaver, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
	Serial.printf("W=%d (Strip mode %d-%d: 0=RGB, 1=RGBW, 2=RGBW with W off) [%s]\r\n",
		user_settings.strip_mode, MIN_STRIP_MODE, MAX_STRIP_MODE, strip_mode_name(user_settings.strip_mode));
	Serial.printf("S=%d (Sound volume %d-%d)\r\n", user_settings.audio_volume, MIN_VOLUME, MAX_VOLUME);
	Serial.printf("D=%d (Joystick deadzone %d-%d)\r\n", user_settings.joystick_deadzone, MIN_JOYSTICK_DEADZONE, MAX_JOYSTICK_DEADZONE);
	Serial.printf("A=%d (Attack sensitivity %d-%d)\r\n", user_settings.attack_threshold, MIN_ATTACK_THRESHOLD, MAX_ATTACK_THRESHOLD);
	Serial.printf("L=%d (Lives per level %d-%d)\r\n", user_settings.lives_per_level, MIN_LIVES_PER_LEVEL, MAX_LIVES_PER_LEVEL);
	
	Serial.println("\r\n(Send...)");
	Serial.println("  ? to show current settings");
	Serial.println("  R to reset everything to defaults");
	Serial.println("  P to reset play statistics");
	Serial.println("  ! to restart ESP");
}

void show_game_stats()
{
	Serial.println("\r\n===== Play statistics ======");
	Serial.print("Games played: ");
	Serial.println(user_settings.games_played);
	if (user_settings.games_played > 0)
	{
		Serial.print("Average Score: ");
		Serial.println(user_settings.total_points / user_settings.games_played);
	}
	Serial.print("High Score: ");
	Serial.println(user_settings.high_score);
	Serial.print("Boss kills: ");
	Serial.println(user_settings.boss_kills);
}

void settings_eeprom_read()
{
	EEPROM.begin(sizeof(user_settings));

	uint8_t ver = EEPROM.read(0);

	if (ver != SETTINGS_VERSION)
	{
		Serial.print("Error: EEPROM settings read failed:");
		Serial.println(ver);
		Serial.println("Loading defaults...");
		EEPROM.end();
		reset_settings();
		return;
	}
	else
	{
		Serial.print("Settings version: ");
		Serial.println(ver);
	}

	EEPROM.readBytes(0, &user_settings, sizeof(user_settings));

	EEPROM.end();
}

void settings_eeprom_write()
{
	sound_pause(); // prevent interrupt from causing crash

	EEPROM.begin(sizeof(user_settings));
	EEPROM.writeBytes(0, &user_settings, sizeof(user_settings));
	EEPROM.commit();
	EEPROM.end();

	sound_resume(); // restore sound interrupt
}

#endif
