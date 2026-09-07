/*
  TWANG32 - An ESP32 port of TWANG
  (c) B. Dring 3/2018
  License: Creative Commons 4.0 Attribution - Share Alike

  TWANG was originally created by Critters
  https://github.com/Critters/TWANG

  It was inspired by Robin Baumgarten's Line Wobbler Game

  Recent Changes
  - Updated to move FastLEDshow to core 0
  Fixed Neopixel
    - Used latest FastLED library..added compile check
    - Updated to move FastLEDshow to core 0
    - Reduced MAX_LEDS on Neopixel
    - Move some defines to a separate config.h file to make them accessible to other files
  - Changed volume typo on serial port settings menu

  Project TODO
    - Make the strip type configurable via serial and wifi


  Usage Notes:
    - Changes to LED strip and other hardware are in config.h
    - Change the strip type to what you are using and compile/load firmware
    - Use Serial port or Wifi to set your strip length.
*/

//

#define VERSION "2025-07-19"

#include <FastLED.h>
#include <Wire.h>
#include "Arduino.h"

// twang files
#include "config.h"
#include "twang_mpu.h"
#include "enemy.h"
#include "particle.h"
#include "spawner.h"
#include "lava.h"
#include "boss.h"
#include "conveyor.h"
#include "iSin.h"
#include "sound.h"
#include "music.h"
#include "settings.h"
#include "wifi_ap.h"
#include "net.h"
#include "samples.h"

#if defined(FASTLED_VERSION) && (FASTLED_VERSION < 3001000)
#error "Requires FastLED 3.1 or later; check github for latest code."
#endif

#define DIRECTION 1
#define USE_GRAVITY 0  // 0/1 use gravity (LED strip going up wall)

// GAME
long previousMillis = 0; // Time of the last redraw

#define TIMEOUT 20000 // time until screen saver in milliseconds

int joystickTilt = 0;   // Stores the angle of the joystick
int joystickWobble = 0; // Stores the max amount of acceleration (wobble)

// WOBBLE ATTACK
#define DEFAULT_ATTACK_WIDTH 70 // Width of the wobble attack, world is 1000 wide
int attack_width = DEFAULT_ATTACK_WIDTH;
#define ATTACK_DURATION 500 // Duration of a wobble attack (ms)
long attackMillis = 0;      // Time the attack started
bool attacking = 0;         // Is the attack in progress?
int attackStartLED = 0, attackEndLED = 0; // leds affected by attack
#define BOSS_WIDTH 40

// TODO all animation durations should be defined rather than literals
// because they are used in main loop and some sounds too.
#define STARTUP_WIPEUP_DUR 200
#define STARTUP_SPARKLE_DUR 1300
#define STARTUP_FADE_DUR 1500

#define GAMEOVER_SPREAD_DURATION 1000
#define GAMEOVER_FADE_DURATION 3000

#define WIN_FILL_DURATION 500 // sound has a freq effect that might need to be adjusted
#define WIN_CLEAR_DURATION 1000
#define WIN_OFF_DURATION 1200

// Main gyro in spring, tracks its connected state and this code will try to
// reconnect it every 2s, if connection drops
Twang_MPU accelgyro = Twang_MPU(Twang_MPU::MPU_ADDR_DEFAULT);
// A "rference" gyro mounted in the base of the case, will be used for delta
// calculation for a better handheld experience (so the case does not need to be
// flat on the ground). Will only be connected once at startup and then ignored
// if later disconnected.
Twang_MPU accelgyro_ref = Twang_MPU(Twang_MPU::MPU_ADDR_ALTERNATIVE);
unsigned long gyroLastCheckMs = 0;
const unsigned long GYRO_CHECK_INTERVAL_MS = 2000;
Samples MPUAngleSamples = {0};
Samples MPUWobbleSamples = {0};

CRGB leds[VIRTUAL_LED_COUNT];
iSin isin = iSin();

// #define JOYSTICK_DEBUG  // comment out to stop serial debugging

// POOLS
#define ENEMY_COUNT 10
Enemy enemyPool[ENEMY_COUNT] = {
    Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy()};

#define PARTICLE_COUNT 100
Particle particlePool[PARTICLE_COUNT] = {
    Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle()};

#define SPAWN_COUNT 5
Spawner spawnPool[SPAWN_COUNT] = {
    Spawner(), Spawner()};

#define LAVA_COUNT 5
Lava lavaPool[LAVA_COUNT] = {
    Lava(), Lava(), Lava(), Lava()};

#define CONVEYOR_COUNT 4
Conveyor conveyorPool[CONVEYOR_COUNT] = {
    Conveyor(), Conveyor()};

Boss boss = Boss();

enum stages
{
    STARTUP,
    PLAY,
    WIN,
    DEAD,
    SCREENSAVER,
    BOSS_KILLED,
    GAMEOVER
} stage;

long stageStartTime;        // Stores the time the stage changed for stages that are time based
int playerPosition;         // Stores the player position
int playerPositionModifier; // +/- adjustment to player position
bool playerAlive;
long killTime;
int lives = LIVES_PER_LEVEL;

#define FASTLED_SHOW_CORE 0 // -- The core to run FastLED.show()

// -- Task handles for use in the notifications
static TaskHandle_t FastLEDshowTaskHandle = 0;
static TaskHandle_t userTaskHandle = 0;

long mapconstrain(long x, long in_min, long in_max, long out_min, long out_max) {
    assert(in_min < in_max);

    if (x <= in_min)
        return out_min;
    if (x >= in_max)
        return out_max;

    const long run = in_max - in_min;
    const long rise = out_max - out_min;
    const long delta = x - in_min;
    return (delta * rise) / run + out_min;
}

/** show() for ESP32
 *  Call this function instead of FastLED.show(). It signals core 0 to issue a show,
 *  then waits for a notification that it is done.
 */
void FastLEDshowESP32()
{
    if (userTaskHandle == 0)
    {
        // -- Store the handle of the current task, so that the show task can
        //    notify it when it's done
        userTaskHandle = xTaskGetCurrentTaskHandle();

        // -- Trigger the show task
        xTaskNotifyGive(FastLEDshowTaskHandle);

        // -- Wait to be notified that it's done
        const TickType_t xMaxBlockTime = pdMS_TO_TICKS(200);
        ulTaskNotifyTake(pdTRUE, xMaxBlockTime);
        userTaskHandle = 0;
    }
}

/** show Task
 *  This function runs on core 0 and just waits for requests to call FastLED.show()
 */
void FastLEDshowTask(void *pvParameters)
{
    // -- Run forever...
    for (;;)
    {
        // -- Wait for the trigger
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // -- Do the show (synchronously)
        FastLED.show();

        // -- Notify the calling task
        xTaskNotifyGive(userTaskHandle);
    }
}

// Set at startup by FastLED.addLeds(), used to switch the strip between the
// 3-channel (RGB) and 4-channel (RGBW) layouts while the game is running.
CLEDController *ledController = NULL;

// Applies user_settings.strip_mode to the LED controller. Cheap and idempotent:
// setRgbw() only stores a small struct that the driver reads on every show(),
// so this can be called from the main loop without cost.
void applyStripMode()
{
    static uint8_t appliedMode = 0xFF; // 0xFF = nothing applied yet

    if (ledController == NULL || appliedMode == user_settings.strip_mode)
        return;

#ifndef USE_NEOPIXEL
    // Clocked strips are 3-channel by construction. Report once and leave the
    // controller alone rather than pushing an RGBW mode it cannot honour.
    if (appliedMode == 0xFF)
        Serial.println("Strip mode: RGB (fixed - clocked strips have no white channel)");
    appliedMode = user_settings.strip_mode;
    return;
#endif

    appliedMode = user_settings.strip_mode;

    switch (user_settings.strip_mode)
    {
    case STRIP_MODE_RGBW:
        ledController->setRgbw(fl::Rgbw(fl::kRGBWDefaultColorTemp, fl::kRGBWExactColors, fl::W3));
        break;
    case STRIP_MODE_RGBW_NO_WHITE:
        ledController->setRgbw(fl::Rgbw(fl::kRGBWDefaultColorTemp, fl::kRGBWNullWhitePixel, fl::W3));
        break;
    case STRIP_MODE_RGB:
    default:
        ledController->setRgbw(fl::RgbwInvalid::value());
        break;
    }

    Serial.printf("Strip mode: %s\r\n", strip_mode_name(user_settings.strip_mode));

    // The number of bytes per pixel on the wire changes with the mode, so
    // anything still lit from the old mode would be left stranded on the strip.
    FastLED.clear(true);
}

// Applies user_settings.power_limit_ma. Same shape as applyStripMode(): cheap,
// idempotent, and only acts when the value actually changed, so it can be
// called from the loop after a remote configuration arrives.
void applyPowerLimit()
{
    static uint16_t applied = 0;
    if (applied == user_settings.power_limit_ma)
        return;
    applied = user_settings.power_limit_ma;

    FastLED.setMaxPowerInVoltsAndMilliamps(POWER_LIMIT_VOLTS, applied);
    Serial.printf("\r\nLED power budget: %d mA @ %d V\r\n", applied, POWER_LIMIT_VOLTS);
}

void setup()
{
    Serial.begin(115200);
    Serial.print("\r\nTWANG32 VERSION: ");
    Serial.println(VERSION);

    // Grund des letzten Starts protokollieren. BROWNOUT heisst: die
    // Versorgungsspannung ist eingebrochen - typisch, wenn ein SK9822 beim
    // Einschalten in seinem zufaelligen Zustand mehrere Ampere zieht, bevor
    // die Firmware ihn loeschen kann. Ohne diese Zeile raet man beim Netzteil
    // herum, und der Verdacht faellt zu Unrecht auf die Verkabelung.
    {
        esp_reset_reason_t grund = esp_reset_reason();
        const char *text =
            grund == ESP_RST_POWERON  ? "Einschalten" :
            grund == ESP_RST_BROWNOUT ? "BROWNOUT - Versorgungsspannung eingebrochen" :
            grund == ESP_RST_SW       ? "Software (z.B. nach OTA)" :
            grund == ESP_RST_PANIC    ? "ABSTURZ" :
            grund == ESP_RST_TASK_WDT ? "Task-Watchdog" :
            grund == ESP_RST_INT_WDT  ? "Interrupt-Watchdog" :
            grund == ESP_RST_EXT      ? "Reset-Taste" : "unbekannt";
        Serial.printf("Startgrund: %s (%d)\r\n", text, (int)grund);
    }

    settings_init(); // load the user settings from EEPROM

    Wire.begin();
    accelgyro.initialize();
    accelgyro.testConnection();
    accelgyro_ref.initialize();
    accelgyro_ref.testConnection();
    gyroLastCheckMs = millis();
    Serial.printf("Main gyro is %sconncted!\r\n", accelgyro.connected ? "" : "NOT ");
    Serial.printf("Reference gyro is %sconncted!\r\n", accelgyro_ref.connected ? "" : "NOT ");

#ifdef USE_NEOPIXEL
    Serial.printf("\r\nCompiled for SK6812 / WS2812B, single wire, data on GPIO %d\r\n", DATA_PIN);
    ledController = &FastLED.addLeds<LED_TYPE, DATA_PIN, LED_COLOR_ORDER>(leds, MAX_LEDS);
#endif

#ifdef USE_SK9822
    Serial.printf("\r\nCompiled for SK9822 / APA102, clocked, data on GPIO %d clock on GPIO %d\r\n",
                  DATA_PIN, CLOCK_PIN);
    ledController = &FastLED.addLeds<LED_TYPE, DATA_PIN, CLOCK_PIN, LED_COLOR_ORDER>(leds, MAX_LEDS);
#endif

    applyStripMode(); // RGB vs RGBW, from the stored user settings

    applyPowerLimit();

    FastLED.setBrightness(user_settings.led_brightness);
    FastLED.setDither(1);

    // -- Create the ESP32 FastLED show task
    xTaskCreatePinnedToCore(FastLEDshowTask, "FastLEDshowTask", 2048, NULL, 2, &FastLEDshowTaskHandle, FASTLED_SHOW_CORE);

    sound_init();
    sound_master_volume(user_settings.audio_volume);

    // Station mode first: ap_setup() only raises the fallback access point if
    // this did not get us onto a network.
    net_begin();
    ap_setup();

    stage = STARTUP;
        if (!music_playing()) music_play(&MUSIC_INTRO); // laeuft noch die Sterbemelodie, hat die Vorrang
    stageStartTime = millis();
    lives = user_settings.lives_per_level;
}

void loop()
{
    long mm = millis();

    settings_param_t param = ap_client_check(); // check for web client
	if (Serial.available())
	{
        // will overwrite if ap page is submitted at the same time, but
        // that will almost never happen and if it does, that's life...
		param = settings_processSerial(Serial.read());
	}
    settings_set(param);
    applyStripMode();   // no-op unless the strip mode actually changed
    applyPowerLimit();  // dito
    sound_master_volume(user_settings.audio_volume);
    if (param.code == 'V' && param.hasValue)
        loadLevel(levelNumber);

    if (stage == PLAY)
    {
        if (attacking)
        {
            SFXattacking();
        }
        else
        {
            SFXtilt(joystickTilt);
        }
    }
    else if (stage == DEAD)
    {
        SFXdead();
    }

    if (mm - previousMillis >= MIN_REDRAW_INTERVAL)
    {
        if (accelgyro.connected)
        {
            getInput();
            if (!accelgyro.connected)
                Serial.println("Gyro disconnected!");
        }
        else if (millis() - gyroLastCheckMs > GYRO_CHECK_INTERVAL_MS)
        {
            accelgyro.initialize();
            accelgyro.testConnection();
            gyroLastCheckMs = millis();
            if (accelgyro.connected)
                Serial.println("Gyro connected!");
        }

        long frameTimer = mm;
        previousMillis = mm;

        if (abs(joystickTilt) > user_settings.joystick_deadzone)
        {
            lastInputTime = mm;
            if (stage == SCREENSAVER)
            {
                levelNumber = -1;
                stageStartTime = mm;
                stage = WIN;
                FastLED.setBrightness(user_settings.led_brightness);
                Serial.println("Woke up from screensaver, going to game...");
            }
        }
        else
        {
            if (lastInputTime + TIMEOUT < mm && stage != SCREENSAVER)
            {
                stage = SCREENSAVER;
                FastLED.setBrightness(user_settings.led_brightnessScreensaver);
                Serial.println("Going to screensaver...");
            }
        }

        if (stage == SCREENSAVER)
        {
            screenSaverTick();
            // The only place network work is allowed. Config polls block for
            // seconds and an OTA download for far longer; doing that mid-game
            // would freeze the strip. Idle is the one moment it costs nothing.
            net_idle_tick();
        }
        else if (stage == STARTUP)
        {
            if (stageStartTime + STARTUP_FADE_DUR > mm)
            {
                tickStartup(mm);
            }
            else
            {
                SFXcomplete();
                levelNumber = 0;
                loadLevel(0);
            }
        }
        else if (stage == PLAY)
        {
            // PLAYING
            if (attacking && attackMillis + ATTACK_DURATION < mm)
                attacking = 0;

            // If not attacking, check if they should be
            if (!attacking && joystickWobble >= user_settings.attack_threshold)
            {
                attackMillis = mm;
                attacking = 1;
            }

            if (attacking)
            {
                attackStartLED = getLED(playerPosition - (attack_width / 2));
                attackEndLED = getLED(playerPosition + (attack_width / 2));
            }

            // If still not attacking, move!
            playerPosition += playerPositionModifier;
            if (!attacking)
            {
                SFXtilt(joystickTilt);
                // int moveAmount = (joystickTilt/6.0);  // 6.0 is ideal at 16ms interval (6.0 / (16.0 / MIN_REDRAW_INTERVAL))
                int moveAmount = (joystickTilt / (6.0)); // 6.0 is ideal at 16ms interval
                if (DIRECTION)
                    moveAmount = -moveAmount;
                moveAmount = constrain(moveAmount, -MAX_PLAYER_SPEED, MAX_PLAYER_SPEED);

                playerPosition -= moveAmount;
                if (playerPosition < 0)
                    playerPosition = 0;

                // stop player from leaving if boss is alive
                if (boss.Alive() && playerPosition >= VIRTUAL_LED_COUNT) // move player back
                    playerPosition = 999;                                //(user_settings.led_count - 1) * (1000.0/user_settings.led_count);

                if (playerPosition >= VIRTUAL_LED_COUNT && !boss.Alive())
                {
                    // Reached exit!
                    levelComplete();
                    return;
                }
            }

            if (inLava(playerPosition))
            {
                die();
            }

            // Ticks and draw calls
            FastLED.clear();
            tickConveyors();
            tickSpawners();
            tickBoss();
            tickLava();
            tickEnemies();
            drawPlayer();
            drawAttack();
            drawExit();
        }
        else if (stage == DEAD)
        {
            // DEAD
            FastLED.clear();
            tickDie(mm);
            if (!tickParticles())
            {
                loadLevel(levelNumber);
            }
        }
        else if (stage == WIN)
        {
            // LEVEL COMPLETE
            tickWin(mm);
        }
        else if (stage == BOSS_KILLED)
        {
            tickBossKilled(mm);
        }
        else if (stage == GAMEOVER)
        {
            if (stageStartTime + GAMEOVER_FADE_DURATION > mm)
            {
                tickGameover(mm);
            }
            else
            {
                FastLED.clear();
                save_game_stats(false); // boss not killed
                score = 0;

                // restart from the beginning
                stage = STARTUP;
        if (!music_playing()) music_play(&MUSIC_INTRO); // laeuft noch die Sterbemelodie, hat die Vorrang
                stageStartTime = millis();
                lives = user_settings.lives_per_level;
            }
        }

        // FastLED.show();
        FastLEDshowESP32();
    }
}

// ---------------------------------
// ------------ LEVELS -------------
// ---------------------------------
enum LEVELS {
    INTRO=0,
    ENEMY_INTRO,
    SPAWNER_INTRO,
    LAVA_INTRO,
    LAVA_MOVING,
    LAVA_SPREADING,
    ENEMY_SIN_INTRO,
    ENEMY_SIN_SWARM,
    LAVA_MOVING_UP,
    CONVEYOR_INTRO,
    CONVEYOR_ENEMIES,
    LAVA_SPREAD_FALL,
    LAVA_RUN,
    CONVEYOR_HALT_TEST,
    SPAWNER_TRAIN,
    SPAWNER_TRAIN_SKINNY,
    SPAWNER_SPLIT,
    SPAWNER_SPLIT_LAVA,
    CONVEYOR_LAVA,
    CONVEYOR_ENEMY_SIN,
    BOSS, // This should always be the last valid level!
};

void loadLevel(int num)
{
    // leave these alone
    updateLives();
    cleanupLevel();
    playerAlive = 1;
    FastLED.setBrightness(user_settings.led_brightness);

    /// Defaults...OK to change the following items in the levels below
    attack_width = DEFAULT_ATTACK_WIDTH;
    playerPosition = 0;

    /* ==== Level Editing Guide ===============
    Level creation is done by adding to or editing the switch statement below.

    Add your level to the enum above, then add a case statement for it in the switch.
    The order of levels in the enum determines their order in game. 

    TWANG uses a virtual 1000 LED grid. It will then scale that number to your strip, so if you
    want something in the middle of your strip use the number 500. Consider the size of your strip
    when adding features. All time values are specified in milliseconds (1/1000 of a second)

    You can add any of the following features.
    See function descriptions (comments above function implementation) for more info.

    spawnEnemy(): You can add up to 10 (ENEMY_COUNT) static or moving enemies.

    spawnSpawner(): This generates and endless source of new enemies. 
      5 (SPAWN_COUNT) pools max

    spawnLava(): You can create 5 (LAVA_COUNT) pools of lava. 
      Lava will toggle on and off in an interval. 
      Lava kills the player and enemies when on.

    spawnConveyor(): You can create 4 (CONVEYOR_COUNT) conveyors. 
      Conveyors move the player at a constant speed.

    ===== Other things you can adjust per level ================

    Player Start position (0..1000):
      playerPosition = xxx; 

    The size of the TWANG attack
      attack_width = xxx;
    */

    if (num < 0 || num > BOSS)
    {
        Serial.printf("ERROR: Unknown level %d. Defaulting to starting level...\r\n", num);
        num = 0;
    }

    switch (num)
    {
    case INTRO: 
        playerPosition = 150;
        break;
    case ENEMY_INTRO:
        spawnEnemy(900, 0, 1, 0);
        break;
    case SPAWNER_INTRO:
        spawnSpawner(950, 4000, 2, 0, -3500);
        break;
    case LAVA_INTRO:
        spawnLava(400, 490, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnEnemy(350, 0, 1, 0);
        spawnSpawner(950, 4500, 3, 0, -3500);
        break;
    case LAVA_MOVING:
        spawnLava(700, 800, 2000, 2000, 0, Lava::OFF, 0, -0.5);
        spawnEnemy(450, 0, 1, 0);
        spawnSpawner(950, 4500, 3, 0, -2000);
        break;
    case LAVA_SPREADING:
        spawnLava(350, 400, 2000, 2000, 0, Lava::OFF, 0.2, 0);
        spawnLava(750, 800, 2000, 2000, 0, Lava::OFF, 0.2, 0);
        spawnEnemy(400, 0, 2, 0);
        spawnEnemy(900, 0, 2, 0);
        break;
    case ENEMY_SIN_INTRO:
        spawnEnemy(700, 1, 7, 275);
        spawnEnemy(500, 1, 5, 250);
        break;
    case ENEMY_SIN_SWARM: 
        spawnEnemy(700, 1, 4, 275); // 425..975
        spawnEnemy(600, 1, 6, 300); // 300..900

        spawnEnemy(800, 1, 6, 200); // 600..1000
        spawnEnemy(450, 1, 5, 300); // 150..750

        spawnEnemy(500, 1, 7, 350); // 150..800
        spawnEnemy(450, 1, 3, 150); // 300..600
        break;
    case LAVA_MOVING_UP:
        playerPosition = 200;
        // TODO: Lava overlapping with spawners seems to be buggy and always kills
        spawnLava(0, 100, 2000, 2000, 0, Lava::OFF, 1, 0);
        spawnEnemy(500, 0, 1, 0);
        spawnSpawner(950, 2500, 3, 0, -1000);
        break;
    case CONVEYOR_INTRO:
        spawnConveyor(100, 450, -3);
        spawnConveyor(550, 900, -6);
        spawnEnemy(950, 0, 1, 0);
        spawnEnemy(500, 0, 1, 0);
        break;
    case CONVEYOR_ENEMIES:
        spawnConveyor(50, 1000, 6);
        spawnEnemy(300, 0, 0, 0);
        spawnEnemy(400, 0, 0, 0);
        spawnEnemy(500, 0, 0, 0);
        spawnEnemy(600, 0, 0, 0);
        spawnEnemy(700, 0, 0, 0);
        spawnEnemy(800, 0, 0, 0);
        spawnEnemy(900, 0, 0, 0);
        break;
    case LAVA_SPREAD_FALL:
        spawnLava(400, 450, 2000, 2000, 0, Lava::OFF, 0.25, -0.5);
        spawnLava(850, 900, 2000, 2000, 0, Lava::OFF, 0.25, -0.5);
        spawnEnemy(350, 0, 1, 0);
        spawnSpawner(950, 4500, 3, 0, 0);
        break;
    case LAVA_RUN:
        spawnLava(200, 300, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnLava(400, 600, 2000, 2000, 0, Lava::ON, 0, 0);
        spawnLava(700, 800, 1250, 750, 0, Lava::OFF, 0, 0);
        spawnSpawner(950, 4000, 4, 0, -2500);
        break;
    case CONVEYOR_HALT_TEST:
        spawnConveyor(100, 400, -4);
        spawnEnemy(450, 0, 0, 0);
        spawnConveyor(500, 800, -6);
        spawnEnemy(850, 0, 0, 0);
        break;
    case SPAWNER_TRAIN:
        spawnEnemy(500, 0, 2, 0);
        spawnSpawner(900, 1300, 2, 0, -1300);
        break;
    case SPAWNER_TRAIN_SKINNY:
        attack_width = 32;
        spawnEnemy(500, 0, 2, 0);
        spawnSpawner(900, 1800, 2, 0, -1800);
        break;
    case SPAWNER_SPLIT:
        spawnSpawner(550, 1500, 2, 0, -1500);
        spawnSpawner(550, 1500, 2, 1, -1500);
        break;
    case SPAWNER_SPLIT_LAVA:
        spawnSpawner(500, 1200, 2, 0, -1200);
        spawnSpawner(500, 1200, 2, 1, -1200);
        spawnLava(900, 950, 2200, 800, 2000, Lava::OFF, 0, 0);
        break;
    case CONVEYOR_LAVA:
        spawnConveyor(100, 300, -4);
        spawnLava(300, 400, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnConveyor(400, 600, -6);
        spawnLava(600, 700, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnConveyor(700, 900, 6);
        spawnLava(900, 990, 3000, 1000, 0, Lava::OFF, 0, 0);
        break;
    case CONVEYOR_ENEMY_SIN:
        spawnEnemy(700, 1, 7, 275);
        spawnEnemy(600, 1, 5, 250);
        spawnEnemy(400, 1, 6, 275);
        spawnSpawner(950, 5500, 4, 0, 3000);
        spawnSpawner(0, 5500, 5, 1, 10000);
        spawnConveyor(100, 900, -4);
        break;
    case BOSS:
        // this should always be the last level
        spawnBoss();
        break;
    }
    lastInputTime = stageStartTime = millis();
    stage = PLAY;
}

void spawnBoss()
{
    boss.Spawn();
    moveBoss();
}

void moveBoss()
{
    int spawnSpeed = 1800;
    if (boss._lives == 2)
        spawnSpeed = 1500;
    if (boss._lives == 1)
        spawnSpeed = 1000;
    spawnPool[0].Spawn(boss._pos, spawnSpeed, 3, 0, 0);
    spawnPool[1].Spawn(boss._pos, spawnSpeed, 3, 1, 0);
}

/* ======================== spawn Functions =====================================

   The following spawn functions add items to pools by looking for an unactive
   item in the pool. You can only add as many as the ..._COUNT. Additonal attemps
   to add will be ignored.

   ==============================================================================
*/
// @param pos: Where the enemy starts (in game coordinates, usually 0..1000)
// @param dir: If it moves, what direction does it go 0=down, 1=away
// @param speed: How fast does it move. Typically 1 to 4.
// @param wobble: 0=regular movement, >0 set length of bouncing back and forth in a sine pattern
void spawnEnemy(int pos, int dir, int speed, int wobble)
{
    for (int e = 0; e < ENEMY_COUNT; e++)
    { // look for one that is not alive for a place to add one
        if (!enemyPool[e].Alive())
        {
            enemyPool[e].Spawn(pos, dir, speed, wobble);
            enemyPool[e].playerSide = pos > playerPosition ? 1 : -1;
            return;
        }
    }
}

// @param pos: The location the enemies with be generated from (in game coordinates, usually 0..1000)
// @param rate_ms: The time in milliseconds between each new enemy
// @param speed: How fast they move. Typically 1 to 4.
// @param dir: Directions they go 0=down, 1=towards goal
// @param startOffset_ms: The delay in milliseconds before the first enemy (added to rate, can be negative)
void spawnSpawner(int pos, int rate_ms, int speed, int dir, int startOffset_ms)
{
    for (int s = 0; s < SPAWN_COUNT; s++)
    {
        if (!spawnPool[s].Alive())
        {
            spawnPool[s].Spawn(pos, rate_ms, speed, dir, startOffset_ms);
            return;
        }
    }
}

// @param left: the lower end of the lava pool (in game coordinates, usually 0..1000)
// @param right: the upper end of the lava pool
// @param ontime: How long the lava stays on in milliseconds
// @param offtime: How long the lava is off in milliseconds
// @param offset: How long (ms) after the level starts before the lava turns on, use this to create patterns with multiple lavas
// @param state: does it start on or off (Lava::ON or Lava::OFF)
// @param grow: This specifies the rate of growth. Use 0 for no growth. Reasonable growth is 0.1 to 0.5
// @param flow: This specifies the rate/direction of flow. Reasonable numbers are 0.2 to 0.8 (positive or negative)
void spawnLava(int left, int right, int ontime, int offtime, int offset, int state, float grow, float flow)
{
    for (int i = 0; i < LAVA_COUNT; i++)
    {
        if (!lavaPool[i].Alive())
        {
            lavaPool[i].Spawn(left, right, ontime, offtime, offset, state, grow, flow);
            return;
        }
    }
}

// @param startPoint: The close end of the conveyor (in game coordinates 0..1000)
// @param endPoint: The far end of the conveyor
// @param dir: positive = away, negative = towards you (must be less than +/- MAX_PLAYER_SPEED=10)
void spawnConveyor(int startPoint, int endPoint, int dir)
{
    for (int i = 0; i < CONVEYOR_COUNT; i++)
    {
        if (!conveyorPool[i]._alive)
        {
            conveyorPool[i].Spawn(startPoint, endPoint, dir);
            return;
        }
    }
}

void cleanupLevel()
{
    for (int i = 0; i < ENEMY_COUNT; i++)
    {
        enemyPool[i].Kill();
    }
    for (int i = 0; i < PARTICLE_COUNT; i++)
    {
        particlePool[i].Kill();
    }
    for (int i = 0; i < SPAWN_COUNT; i++)
    {
        spawnPool[i].Kill();
    }
    for (int i = 0; i < LAVA_COUNT; i++)
    {
        lavaPool[i].Kill();
    }
    for (int i = 0; i < CONVEYOR_COUNT; i++)
    {
        conveyorPool[i].Kill();
    }
    boss.Kill();
}

void levelComplete()
{
    stageStartTime = millis();
    stage = WIN;

    if (levelNumber == BOSS)
    {
        stage = BOSS_KILLED;
    }
    if (levelNumber != 0) // no points for the first level
    {
        score = score + (lives * 10); //
    }
}

void nextLevel()
{
    levelNumber++;

    if (levelNumber > BOSS)
    {
        levelNumber = 0;
    }

    loadLevel(levelNumber);
    lives = user_settings.lives_per_level;
}

void die()
{
    playerAlive = 0;
    if (levelNumber > 0)
        lives--;

    if (lives == 0)
    {
        stage = GAMEOVER;
        music_play(&MUSIC_DEAD);
        stageStartTime = millis();
    }
    else
    {
        for (int p = 0; p < PARTICLE_COUNT; p++)
        {
            particlePool[p].Spawn(playerPosition);
        }
        stageStartTime = millis();
        stage = DEAD;
    }
    killTime = millis();
}

// ----------------------------------
// -------- TICKS & RENDERS ---------
// ----------------------------------
void tickStartup(long mm)
{
    FastLED.clear();
    // Der Aufwaertssweep und die Fanfare wuerden gleichzeitig laufen. Die
    // Musik hat Vorrang; der Sweep kommt zurueck, sobald sie durch ist.
    if (!music_playing())
        SFXFreqSweepWarble(STARTUP_FADE_DUR, millis() - stageStartTime, 40, 400, 20);
    // temporarily reduce brightness, since full strip will light up, which is much brighter in total
    FastLED.setBrightness(user_settings.led_brightness / 4);
    if (stageStartTime + STARTUP_WIPEUP_DUR > mm) // fill to the top with green
    {
        int n = mapconstrain(mm - stageStartTime, 0, STARTUP_WIPEUP_DUR, user_settings.led_offset, user_settings.led_end); // fill from top to bottom
        for (int i = user_settings.led_offset; i < n; i++)
        {
            leds[i] = CRGB(0, 255, 0);
        }
    }
    else if (stageStartTime + STARTUP_SPARKLE_DUR > mm) // sparkle the full green bar
    {
        FOREACH_LED(i)
        {
            if (random8(30) < 28)
                leds[i] = CRGB(0, 255, 0); // most are green
            else
            {
                int flicker = random8(250);
                leds[i] = CRGB(flicker, 150, flicker); // some flicker brighter
            }
        }
    }
    else if (stageStartTime + STARTUP_FADE_DUR > mm) // fade it out to bottom
    {
        int n = mapconstrain(mm - stageStartTime, STARTUP_SPARKLE_DUR, STARTUP_FADE_DUR, user_settings.led_offset, user_settings.led_end); // fill from top to bottom
        int brightness = mapconstrain(mm - stageStartTime, STARTUP_SPARKLE_DUR, STARTUP_FADE_DUR, 255, 0);

        for (int i = n; i < user_settings.led_end; i++)
        {
            leds[i] = CRGB(0, brightness, 0);
        }
    }
    // Der Sweep steht jetzt oben in dieser Funktion, hinter der Musikpruefung.
}

void tickEnemies()
{
    int attStart = map(attackStartLED, user_settings.led_offset, user_settings.led_end - 1, 0, VIRTUAL_LED_COUNT);
    // making sure to cover the full range of the LED in virtual game space
    int attEnd = map(attackEndLED + 1, user_settings.led_offset, user_settings.led_end - 1, 0, VIRTUAL_LED_COUNT) - 1;

    for (int i = 0; i < ENEMY_COUNT; i++)
    {
        if (enemyPool[i].Alive())
        {
            enemyPool[i].Tick();
            // Hit attack?
            if (attacking)
            {
                if (enemyPool[i]._pos >= attStart && enemyPool[i]._pos <= attEnd)
                {
                    enemyPool[i].Kill();
                    SFXkill();
                }
            }
            if (inLava(enemyPool[i]._pos))
            {
                enemyPool[i].Kill();
                SFXkill();
            }
            // Draw (if still alive)
            if (enemyPool[i].Alive())
            {
                leds[getLED(enemyPool[i]._pos)] = CRGB(255, 0, 0);
            }
            // Hit player?
            if (
                (enemyPool[i].playerSide == 1 && enemyPool[i]._pos <= playerPosition) ||
                (enemyPool[i].playerSide == -1 && enemyPool[i]._pos >= playerPosition))
            {
                die();
                return;
            }
        }
    }
}

void tickBoss()
{
    // DRAW
    if (boss.Alive())
    {
        boss._ticks++;
        for (int i = getLED(boss._pos - BOSS_WIDTH / 2); i <= getLED(boss._pos + BOSS_WIDTH / 2); i++)
        {
            leds[i] = CRGB::DarkRed;
            leds[i] %= 100;
        }
        // CHECK COLLISION
        if (getLED(playerPosition) > getLED(boss._pos - BOSS_WIDTH / 2) && getLED(playerPosition) < getLED(boss._pos + BOSS_WIDTH))
        {
            die();
            return;
        }
        // CHECK FOR ATTACK
        if (attacking)
        {
            bool attackStartInsideBoss = attackStartLED <= getLED(boss._pos + BOSS_WIDTH / 2) && attackStartLED >= getLED(boss._pos - BOSS_WIDTH / 2);
            bool attackEndInsideBoss   = attackEndLED   <= getLED(boss._pos + BOSS_WIDTH / 2) && attackEndLED   >= getLED(boss._pos - BOSS_WIDTH / 2);
            if (attackStartInsideBoss || attackEndInsideBoss)
            {
                boss.Hit();
                if (boss.Alive())
                {
                    moveBoss();
                }
                else
                {
                    spawnPool[0].Kill();
                    spawnPool[1].Kill();
                }
            }
        }
    }
}

void drawPlayer()
{
    leds[getLED(playerPosition)] = CRGB(0, 255, 0);
}

void drawExit()
{
    if (!boss.Alive())
    {
        leds[user_settings.led_end - 1] = CRGB(0, 0, 255);
    }
}

void tickSpawners()
{
    const CRGB defaultCol = CRGB(LAVA_OFF_BRIGHTNESS, LAVA_OFF_BRIGHTNESS / 1.5, 0);
    const CRGB warnCol = CRGB(LAVA_OFF_BRIGHTNESS * 2, LAVA_OFF_BRIGHTNESS * 2, 0);
    unsigned long mm = millis();
    for (int s = 0; s < SPAWN_COUNT; s++)
    {
        if (!spawnPool[s].Alive())
            continue;
        if (mm - spawnPool[s]._lastSpawned > spawnPool[s]._rate + spawnPool[s]._delayOnce)
        {
            spawnEnemy(spawnPool[s]._pos, spawnPool[s]._dir, spawnPool[s]._sp, 0);
            spawnPool[s]._lastSpawned = mm;
            spawnPool[s]._delayOnce = 0;
        }
        long nextSpawn = spawnPool[s]._lastSpawned + spawnPool[s]._rate + spawnPool[s]._delayOnce;
        if (nextSpawn - mm < 800)
        {
            leds[getLED(spawnPool[s]._pos)] = (nextSpawn - mm) % 200 < 100 ? defaultCol : warnCol;
        }
        else
        {
            leds[getLED(spawnPool[s]._pos)] = defaultCol;
        }
    }
}

void tickLava()
{
    int A, B, p, i, brightness, flicker;
    long mm = millis();

    Lava LP;
    for (i = 0; i < LAVA_COUNT; i++)
    {
        LP = lavaPool[i];
        if (LP.Alive())
        {
            LP.Update(); // for grow and flow
            A = getLED(LP._left);
            B = getLED(LP._right);
            if (LP._state == Lava::OFF)
            {
                if (LP._lastOn + LP._offtime < mm)
                {
                    LP._state = Lava::ON;
                    LP._lastOn = mm;
                }
                for (p = A; p <= B; p++)
                {
                    flicker = random8(LAVA_OFF_BRIGHTNESS);
                    leds[p] = CRGB(LAVA_OFF_BRIGHTNESS + flicker, (LAVA_OFF_BRIGHTNESS + flicker) / 1.5, 0);
                }
            }
            else if (LP._state == Lava::ON)
            {
                if (LP._lastOn + LP._ontime < mm)
                {
                    LP._state = Lava::OFF;
                    LP._lastOn = mm;
                }
                for (p = A; p <= B; p++)
                {
                    if (random8(30) < 29)
                        leds[p] = CRGB(150, 0, 0);
                    else
                        leds[p] = CRGB(180, 100, 0);
                }
            }
        }
        lavaPool[i] = LP;
    }
}

bool tickParticles()
{
    bool stillActive = false;
    uint8_t brightness;
    for (int p = 0; p < PARTICLE_COUNT; p++)
    {
        if (particlePool[p].Alive())
        {
            particlePool[p].Tick();

            if (particlePool[p]._power < 5)
            {
                brightness = (5 - particlePool[p]._power) * 10;
                leds[getLED(particlePool[p]._pos)] += CRGB(brightness, brightness / 2, brightness / 2);
            }
            else
                leds[getLED(particlePool[p]._pos)] += CRGB(particlePool[p]._power, 0, 0);

            stillActive = true;
        }
    }
    return stillActive;
}

void tickConveyors()
{
    long m = 10000 + millis();
    playerPositionModifier = 0;

    static const int levels = 5; // brightness levels in conveyor
    // For WS2812 LEDs this looks good with user_settings.led_brightness > 50
    // for lower led_brightness values you might want to increase 
    // MIN_BRIGHTNESS and CONVEYOR_BRIGHTNESS
    static const int brightnessMap[levels] = {
        MIN_BRIGHTNESS, 
        MIN_BRIGHTNESS + (CONVEYOR_BRIGHTNESS - MIN_BRIGHTNESS) / 12,
        MIN_BRIGHTNESS + (CONVEYOR_BRIGHTNESS - MIN_BRIGHTNESS) / 6,
        MIN_BRIGHTNESS + (CONVEYOR_BRIGHTNESS - MIN_BRIGHTNESS) / 3,
        CONVEYOR_BRIGHTNESS,
    };

    for (int i = 0; i < CONVEYOR_COUNT; i++)
    {
        if (! conveyorPool[i]._alive)
            continue;

        int firstLed = getLED(conveyorPool[i]._startPoint);
        int lastLed = getLED(conveyorPool[i]._endPoint);
        for (int led = firstLed; led <= lastLed; led++)
        {
            // this results in -4..0 for conveyors moving towards start
            // and 0..4 for conveyors moving towards goal with each led
            // being +-1 off the last one
            // conveyors with speed +-5 will "move" at 100ms per LED
            int n = (-led + (m * conveyorPool[i]._speed / 500)) % levels;
            // make all values positive for value mapping
            n = abs(n);

            assert(n >= 0 && n < levels);

            int b = brightnessMap[n];
            leds[led] = CRGB(b, b, b);
        }

        if (playerPosition >= conveyorPool[i]._startPoint && playerPosition <= conveyorPool[i]._endPoint)
        {
            playerPositionModifier = conveyorPool[i]._speed;
        }
    }
}

void tickComplete(long mm) // the boss is dead
{
    int brightness = 0;
    FastLED.clear();
    SFXcomplete();
    if (stageStartTime + 500 > mm)
    {
        int n = mapconstrain(mm - stageStartTime, 0, 500, user_settings.led_end, user_settings.led_offset);
        for (int i = user_settings.led_end-1; i > n; i--)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
    }
    else if (stageStartTime + 5000 > mm)
    {
        for (int i = user_settings.led_end-1; i >= user_settings.led_offset; i--)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
    }
    else if (stageStartTime + 5500 > mm)
    {
        int n = mapconstrain(mm - stageStartTime, 5000, 5500, user_settings.led_end, user_settings.led_offset);
        for (int i = user_settings.led_offset; i < n; i++)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
    }
    else
    {
        nextLevel();
    }
}

void tickBossKilled(long mm) // boss funeral
{
    static uint8_t gHue = 0;

    FastLED.setBrightness(min(user_settings.led_brightness * 2, MAX_BRIGHTNESS)); // super bright!

    int brightness = 0;
    FastLED.clear();

    if (stageStartTime + 6500 > mm)
    {
        gHue++;
        fill_rainbow(leds + user_settings.led_offset, LED_LENGTH, gHue, 7); // FastLED's built in rainbow
        if (random8() < 200)
        { // add glitter
            int idx = user_settings.led_offset + random16(LED_LENGTH - 1);
            leds[idx] += CRGB::White;
        }
        SFXbosskilled();
    }
    else if (stageStartTime + 7000 > mm)
    {
        int n = mapconstrain(mm - stageStartTime, 5000, 5500, user_settings.led_end, user_settings.led_offset);
        for (int i = user_settings.led_offset; i < n; i++)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
        SFXcomplete();
    }
    else
    {
        FastLED.setBrightness(user_settings.led_brightness);
        stage = STARTUP;
        if (!music_playing()) music_play(&MUSIC_INTRO); // laeuft noch die Sterbemelodie, hat die Vorrang
        stageStartTime = millis();
        save_game_stats(true);
        lives = user_settings.lives_per_level;
    }
}

void tickDie(long mm)
{                             // a short bright explosion...particles persist after it.
    const int duration = 200; // milliseconds
    const int width = 20;     // half width of the explosion

    if (stageStartTime + duration > mm)
    { // Spread red from player position up and down the width

        int brightness = map(mm - stageStartTime, 0, duration, 255, 150); // this allows a fade from white to red

        // fill up
        int n = mapconstrain(mm - stageStartTime, 0, duration, getLED(playerPosition), getLED(playerPosition) + width);
        for (int i = getLED(playerPosition); i <= n; i++)
        {
            leds[i] = CRGB(255, brightness, brightness);
        }

        // fill to down
        n = mapconstrain(mm - stageStartTime, 0, duration, getLED(playerPosition), getLED(playerPosition) - width);
        for (int i = getLED(playerPosition); i >= n; i--)
        {
            leds[i] = CRGB(255, brightness, brightness);
        }
    }
}

void tickGameover(long mm)
{
    int brightness = 0;
    // temporarily reduce brightness, since full strip will light up, which is much brighter in total
    FastLED.setBrightness(user_settings.led_brightness / 4);

    if (stageStartTime + GAMEOVER_SPREAD_DURATION > mm) // Spread red from player position to top and bottom
    {
        // fill to top
        int n = mapconstrain(mm - stageStartTime, 0, GAMEOVER_SPREAD_DURATION, getLED(playerPosition), user_settings.led_end);
        for (int i = getLED(playerPosition); i < n; i++)
        {
            leds[i] = CRGB(255, 0, 0);
        }
        // fill to bottom
        n = mapconstrain(mm - stageStartTime, 0, GAMEOVER_SPREAD_DURATION, getLED(playerPosition), user_settings.led_offset);
        for (int i = getLED(playerPosition); i >= n; i--)
        {
            leds[i] = CRGB(255, 0, 0);
        }
        SFXgameover();
    }
    else if (stageStartTime + GAMEOVER_FADE_DURATION > mm) // fade brightness
    {
        brightness = mapconstrain(mm - stageStartTime, GAMEOVER_SPREAD_DURATION, GAMEOVER_FADE_DURATION-500, 255, 0);

        FOREACH_LED(i)
        {
            leds[i] = CRGB(brightness, 0, 0);
        }
        SFXcomplete();
    }
}

void tickWin(long mm)
{
    FastLED.clear();
    // temporarily reduce brightness, since full strip will light up, which is much brighter in total
    FastLED.setBrightness(user_settings.led_brightness / 4);
    if (stageStartTime + WIN_FILL_DURATION > mm)
    {
        int n = mapconstrain(mm - stageStartTime, 0, WIN_FILL_DURATION, user_settings.led_end, user_settings.led_offset); // fill from top to bottom
        for (int i = user_settings.led_end-1; i >= n; i--)
        {
            leds[i] = CRGB(0, 255, 0);
        }
        SFXwin();
    }
    else if (stageStartTime + WIN_CLEAR_DURATION > mm)
    {
        int n = mapconstrain(mm - stageStartTime, WIN_FILL_DURATION, WIN_CLEAR_DURATION, user_settings.led_end, user_settings.led_offset); // clear from top to bottom
        for (int i = user_settings.led_offset; i < n; i++)
        {
            leds[i] = CRGB(0, 255, 0);
        }
        SFXwin();
    }
    else if (stageStartTime + WIN_OFF_DURATION > mm)
    { 
        // wait a while with leds off
    }
    else
    {
        nextLevel();
    }
}

void drawLives()
{
    // show how many lives are left by drawing a short line of green leds for each life
    SFXcomplete(); // stop any sounds
    FastLED.clear();

    static const int ledsPerLife = 4;

    int pos = user_settings.led_offset;
    for (int i = 0; i < lives; i++)
    {
        for (int j = 0; j < ledsPerLife; j++)
        {
            leds[pos++] = CRGB(0, 255, 0);
            FastLEDshowESP32();
        }
        leds[pos++] = CRGB(0, 0, 0);
        leds[pos++] = CRGB(0, 0, 0);
        delay(30);
    }
    FastLEDshowESP32();
    delay(500);
    FastLED.clear();
}

void drawAttack()
{
    if (!attacking)
        return;
    int n = map(millis() - attackMillis, 0, ATTACK_DURATION, 100, 5);
    for (int i = attackStartLED + 1; i <= attackEndLED - 1; i++)
    {
        leds[i] = CRGB(0, 0, n);
    }
    if (n > 90)
    {
        n = 255;
        leds[getLED(playerPosition)] = CRGB(255, 255, 255);
    }
    else
    {
        n = 0;
        leds[getLED(playerPosition)] = CRGB(0, 255, 0);
    }
    leds[attackStartLED] = CRGB(n, n, 255);
    leds[attackEndLED] = CRGB(n, n, 255);
}

int getLED(int pos)
{
    // The world is 1000 pixels wide, this converts world units into an LED number
    return mapconstrain(pos, 0, VIRTUAL_LED_COUNT, user_settings.led_offset, user_settings.led_end - 1);
}

bool inLava(int pos)
{
    // Returns if the player is in active lava
    int i;
    Lava LP;
    for (i = 0; i < LAVA_COUNT; i++)
    {
        LP = lavaPool[i];
        if (LP.Alive() && LP._state == Lava::ON)
        {
            if (LP._left <= pos && LP._right >= pos)
                return true;
        }
    }
    return false;
}

void updateLives()
{
    drawLives();
}

void save_game_stats(bool bossKill)
{
    user_settings.games_played += 1;
    user_settings.total_points += score;
    if (score > user_settings.high_score)
        user_settings.high_score = score;
    if (bossKill)
        user_settings.boss_kills += 1;

    show_game_stats();
    settings_eeprom_write();
}

// ---------------------------------
// ----------- JOYSTICK ------------
// ---------------------------------
// returns success (if at least the main gyro could be read)
bool getInput()
{
    // This is responsible for the player movement speed and attacking.
    // You can replace it with anything you want that passes a -90..90 value to joystickTilt
    // and any value to joystickWobble that is >= than ATTACK_THRESHOLD (defined at start)
    // For example you could use 3 momentary buttons:
    // if(digitalRead(leftButtonPinNumber) == HIGH) joystickTilt = -90;
    // if(digitalRead(rightButtonPinNumber) == HIGH) joystickTilt = 90;
    // if(digitalRead(attackButtonPinNumber) == HIGH) joystickWobble = ATTACK_THRESHOLD;

    bool connected = accelgyro.getMotion6();
    if (!connected)
    {
        // Bail out without leaving stale input behind. sample_highest() returns
        // the MAXIMUM of the last few samples, so a spike captured just before
        // the gyro dropped out would otherwise never age out: the attack ends
        // after ATTACK_DURATION, is immediately re-triggered by the stale
        // value, and since movement only happens while not attacking, the
        // player is stuck attacking forever. Fail to "no input" instead.
        sample_fill(&MPUWobbleSamples, 0);
        sample_fill(&MPUAngleSamples, 0);
        joystickWobble = 0;
        joystickTilt = 0;
        return false;
    }

    int a = (JOYSTICK_ORIENTATION == 0 ? accelgyro.ax : (JOYSTICK_ORIENTATION == 1 ? accelgyro.ay : accelgyro.az)) / 166;
    int g = (JOYSTICK_ORIENTATION == 0 ? accelgyro.gx : (JOYSTICK_ORIENTATION == 1 ? accelgyro.gy : accelgyro.gz));
    int ra = 0;
    int rg = 0;
    
    if (accelgyro_ref.connected)
    {
        connected = accelgyro_ref.getMotion6();
        if (connected)
        {
            ra = (JOYSTICK_ORIENTATION == 0 ? accelgyro_ref.ax : (JOYSTICK_ORIENTATION == 1 ? accelgyro_ref.ay : accelgyro_ref.az)) / 166;
            rg = (JOYSTICK_ORIENTATION == 0 ? accelgyro_ref.gx : (JOYSTICK_ORIENTATION == 1 ? accelgyro_ref.gy : accelgyro_ref.gz));
            a -= ra;
            g -= rg;
        }
    }

    if (abs(a) < user_settings.joystick_deadzone)
        a = 0;
    if (a > 0)
        a -= user_settings.joystick_deadzone;
    if (a < 0)
        a += user_settings.joystick_deadzone;
    sample_add(&MPUAngleSamples, a);
    sample_add(&MPUWobbleSamples, g);

    joystickTilt = sample_median(&MPUAngleSamples);
    if (JOYSTICK_DIRECTION == 1)
    {
        joystickTilt = 0 - joystickTilt;
    }
    joystickWobble = abs(sample_highest(&MPUWobbleSamples));

#ifdef JOYSTICK_DEBUG
    static unsigned long lastInputPrint = 0;
    #define PRINT_INTERVAL 500
    if (millis() - lastInputPrint > PRINT_INTERVAL)
    {
        Serial.printf("Joystick  - a = (%6d, %6d, %6d), g = (%6d, %6d, %6d)\n", 
                accelgyro.ax, accelgyro.ay, accelgyro.az, accelgyro.gx, accelgyro.gy, accelgyro.gz);
        Serial.printf("Reference - a = (%6d, %6d, %6d), g = (%6d, %6d, %6d)\n", 
                accelgyro_ref.ax, accelgyro_ref.ay, accelgyro_ref.az, accelgyro_ref.gx, accelgyro_ref.gy, accelgyro_ref.gz);

        Serial.printf("Result: a = %6d, g = %6d, tilt = %6d, wobble = %6d\n", a, g, joystickTilt, joystickWobble);
        lastInputPrint = millis();
    }
#endif

    return true;
}

// ---------------------------------
// -------------- SFX --------------
// ---------------------------------

/*
   This is used sweep across (up or down) a frequency range for a specified duration.
   A sin based warble is added to the frequency. This function is meant to be called
   on each frame to adjust the frequency in sync with an animation

   duration 	= over what time period is this mapped
   elapsedTime 	= how far into the duration are we in
   freqStart 	= the beginning frequency
   freqEnd 		= the ending frequency
   warble 		= the amount of warble added (0 disables)


*/
void SFXFreqSweepWarble(int duration, int elapsedTime, int freqStart, int freqEnd, int warble)
{
    int freq = map_constrain(elapsedTime, 0, duration, freqStart, freqEnd);
    if (warble)
        warble = map(sin(millis() / 20.0) * 1000.0, -1000, 1000, 0, warble);

    sound(freq + warble, user_settings.audio_volume);
}

/*

   This is used sweep across (up or down) a frequency range for a specified duration.
   Random noise is optionally added to the frequency. This function is meant to be called
   on each frame to adjust the frequency in sync with an animation

   duration 	= over what time period is this mapped
   elapsedTime 	= how far into the duration are we in
   freqStart 	= the beginning frequency
   freqEnd 		= the ending frequency
   noiseFactor 	= the amount of noise to added/subtracted (0 disables)


*/
void SFXFreqSweepNoise(int duration, int elapsedTime, int freqStart, int freqEnd, uint8_t noiseFactor)
{
    int freq;

    if (elapsedTime > duration)
        freq = freqEnd;
    else
        freq = map(elapsedTime, 0, duration, freqStart, freqEnd);

    if (noiseFactor)
        noiseFactor = noiseFactor - random8(noiseFactor / 2);

    sound(freq + noiseFactor, user_settings.audio_volume);
}

// How rough the movement tone is, and how often it is re-tuned.
//
// Upstream added random8(100) to the frequency and let the main loop call this
// every frame, so the pitch was re-rolled by up to 100 Hz about sixty times a
// second. On a piezo that reads as a rough engine hum; through a class-D amp
// into an efficient driver it is a rattle. Two things cause it, and both are
// fixed here: the size of the random jump, and its rate.
//
// Re-tuning also rewrites the hardware timer's alarm, which truncates the half
// period currently being output. Doing that every frame roughens the waveform
// on top of the intended randomness.
#define TILT_SFX_WOBBLE 15   // Hz of random detune (upstream: 100)
#define TILT_SFX_UPDATE_MS 80 // how often to re-tune (upstream: every frame)

void SFXtilt(int amount)
{
    if (amount == 0)
    {
        SFXcomplete();
        return;
    }

    // Between updates the tone simply keeps playing: the timer is already
    // running at the right frequency, so returning early is what makes it
    // steady rather than jittery.
    static unsigned long lastTiltSFX = 0;
    static uint8_t tiltWobble = 0;
    unsigned long now = millis();
    if (now - lastTiltSFX < TILT_SFX_UPDATE_MS)
        return;
    lastTiltSFX = now;
    tiltWobble = random8(TILT_SFX_WOBBLE);

    int f = map(abs(amount), 0, 90, 80, 900) + tiltWobble;
    if (playerPositionModifier < 0)
        f -= 500;
    if (playerPositionModifier > 0)
        f += 200;
    int vol = map(abs(amount), 0, 90, user_settings.audio_volume / 2, user_settings.audio_volume * 3 / 4);
    sound(f, vol);
}
void SFXattacking()
{
    int freq = map(sin(millis() / 2.0) * 1000.0, -1000, 1000, 500, 600);
    if (random8(5) == 0)
    {
        freq *= 3;
    }
    sound(freq, user_settings.audio_volume);
}
void SFXdead()
{
    SFXFreqSweepNoise(1000, millis() - killTime, 1000, 10, 200);
}

void SFXgameover()
{
    SFXFreqSweepWarble(GAMEOVER_SPREAD_DURATION, millis() - killTime, 440, 20, 60);
}

void SFXkill()
{
    sound(2000, user_settings.audio_volume);
}
void SFXwin()
{
    SFXFreqSweepWarble(WIN_OFF_DURATION, millis() - stageStartTime, 40, 400, 20);
}

void SFXbosskilled()
{
    SFXFreqSweepWarble(7000, millis() - stageStartTime, 75, 1100, 60);
}

void SFXcomplete()
{
    soundOff();
}

/*
  This works just like the map function except x is constrained to the range of in_min and in_max
*/
long map_constrain(long x, long in_min, long in_max, long out_min, long out_max)
{
    // constain the x value to be between in_min and in_max
    if (in_max > in_min)
    { // map allows min to be larger than max, but constrain does not
        x = constrain(x, in_min, in_max);
    }
    else
    {
        x = constrain(x, in_max, in_min);
    }
    return map(x, in_min, in_max, out_min, out_max);
}

// ---------------------------------
// --------- SCREENSAVER -----------
// ---------------------------------
#define SCREENSAVER_DURATION_MS 60000

// These will be played in order based on total on-time
// Taking SCREENSAVER_DURATION_MS each
// The placeholders default to off, to reduce battery usage
// and make screensavers less "busy"
typedef enum Screensavers
{
    FIRE,
    PLACHOLDER_OFF1,
    PLACEHOLDER_OFF2,
    SINELON,
    JUGGLE,
    PLACHOLDER_OFF3,
    PLACEHOLDER_OFF4,
    // These three are not as nice looking, so removed for now
    //COLOR_WIPE,
    //COLOR_WHEEL,
    //COLOR_CIRCLE,
    LED_MARCH,
    RANDOM_FLASHES,

    SAVE_EOL
} Screensavers;

void screenSaverTick()
{
    long mm = millis();
    Screensavers mode = Screensavers((mm / SCREENSAVER_DURATION_MS) % SAVE_EOL);

    SFXcomplete(); // turn off sound...play testing showed this to be a problem

    FastLED.setBrightness(user_settings.led_brightnessScreensaver);

    switch (mode)
    {
    case FIRE: FastLED.setBrightness(user_settings.led_brightnessScreensaver / 3); Fire2012(); break;
    case SINELON: sinelon(); break;
    case JUGGLE: juggle(); break;
    case LED_MARCH: LED_march(); break;
    // Disabled together with their enum entries above (upstream left these
    // cases in place, which breaks the build).
    //case COLOR_WIPE: colorWipes(); break;
    //case COLOR_WHEEL: colorWheel(); break;
    //case COLOR_CIRCLE: colorCircle(); break;
    case RANDOM_FLASHES: random_LED_flashes(); break;
    default: fadeToBlack(10); break; // for PLACEHOLDER_OFF and unknown states
    }
}

// Fire2012 by Mark Kriegsman, July 2012
// as part of "Five Elements" shown here: http://youtu.be/knWiGsmgycY
////
// This basic one-dimensional 'fire' simulation works roughly as follows:
// There's a underlying array of 'heat' cells, that model the temperature
// at each point along the line.  Every cycle through the simulation,
// four steps are performed:
//  1) All cells cool down a little bit, losing heat to the air
//  2) The heat from each cell drifts 'up' and diffuses a little
//  3) Sometimes randomly new 'sparks' of heat are added at the bottom
//  4) The heat from each cell is rendered as a color into the leds array
//     The heat-to-color mapping uses a black-body radiation approximation.
//
// Temperature is in arbitrary units from 0 (cold black) to 255 (white hot).
//
// This simulation scales it self a bit depending on NUM_LEDS; it should look
// "OK" on anywhere from 20 to 100 LEDs without too much tweaking.
//
// I recommend running this simulation at anywhere from 30-100 frames per second,
// meaning an interframe delay of about 10-35 milliseconds.
//
// Looks best on a high-density LED setup (60+ pixels/meter).
//
//
// There are two main parameters you can play with to control the look and
// feel of your fire: COOLING (used in step 1 above), and SPARKING (used
// in step 3 above).
//
// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100
#define COOLING 100

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 120

//======================================== SCREEN SAVERS =================

void Fire2012()
{
    // Array of temperature readings at each simulation cell
    static byte heat[VIRTUAL_LED_COUNT]; // the most possible
    bool gReverseDirection = false;

    // Step 1.  Cool down every cell a little
    FOREACH_LED(i)
    {
        heat[i] = qsub8(heat[i], random8(0, ((COOLING * 10) / LED_LENGTH) + 2));
    }

    // Step 2.  Heat from each cell drifts 'up' and diffuses a little
    for (int k = user_settings.led_end - 1; k >= user_settings.led_offset + 2; k--)
    {
        heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
    }

    // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
    if (random8() < SPARKING)
    {
        int y = user_settings.led_offset + random8(7);
        heat[y] = qadd8(heat[y], random8(160, 255));
    }

    // Step 4.  Map from heat cells to LED colors
    FOREACH_LED(j)
    {
        CRGB color = HeatColor(heat[j]);
        int pixelnumber;
        if (gReverseDirection)
        {
            pixelnumber = (user_settings.led_end - 1) - j;
        }
        else
        {
            pixelnumber = j;
        }
        leds[pixelnumber] = color;
    }
}

void LED_march()
{
    long mm = millis();

    FOREACH_LED(i)
    {
        leds[i].nscale8(250);
    }

    // Marching green <> orange
    int n = (mm / 250) % 10;
    int b = 10 + ((sin(mm / 500.00) + 1) * 20.00);
    int c = 20 + ((sin(mm / 5000.00) + 1) * 33);
    FOREACH_LED(i)
    {
        if (i % 10 == n)
        {
            leds[i] = CHSV(c, 255, 150);
        }
    }
}

void random_LED_flashes()
{
    long mm = millis();

    FOREACH_LED(i)
    {
        leds[i].nscale8(250);
    }

    randomSeed(mm);
    FOREACH_LED(i)
    {
        if (random8(20) == 0)
        {
            leds[i] = CHSV(25, 255, 100);
        }
    }
}

void sinelon()
{
    // this is necessary so we don't fill the whole strip after switching animations
    // NOTE (JS, 11.07.25): Only tested with 300 leds, might need increasing for 1000
    const static int MAX_POS_DIFF_TO_BRIDGE = 16;
    static int oldPos = 0;

    static uint8_t gHue = 0; // rotating "base color" used by many of the patterns
    gHue++;

    // a colored dot sweeping back and forth, with fading trails
    fadeToBlackBy(leds + user_settings.led_offset, LED_LENGTH, 20);
    int pos = beatsin16(13, user_settings.led_offset, user_settings.led_end - 1);
    // On big strips dots move very fast, leaving gaps between their old and new
    // position, we fill these in with linear interpolated values
    // does not properly track the "bounces" at the end, but you barely notice that
    if (abs(pos - oldPos) <= MAX_POS_DIFF_TO_BRIDGE)
    {
        int smaller = pos < oldPos ? pos : oldPos;
        int bigger =  pos > oldPos ? pos : oldPos;
        for (int pdx = smaller+1; pdx < bigger; ++pdx)
        {
            float scale = 1.f - 20.f/256.f * (pdx - oldPos) / (pos - oldPos);
            leds[pdx] += CHSV(gHue * scale, 255 * scale, 192 * scale);
        }
    }
    leds[pos] += CHSV(gHue, 255, 192);
    oldPos = pos;
}

void juggle()
{
    const static int DOT_COUNT = 4;
    // this is necessary so we don't fill the whole strip after switching animations
    // NOTE (JS, 11.07.25): Only tested with 300 leds, might need increasing for 1000
    const static int MAX_POS_DIFF_TO_BRIDGE = 16;
    static int oldPos[DOT_COUNT] = {0};
    int pos[DOT_COUNT] = {0};

    // colored dots, weaving in and out of sync with each other
    fadeToBlackBy(leds + user_settings.led_offset, LED_LENGTH, 20);
    byte dothue = 0;
    for (int i = 0; i < DOT_COUNT; i++)
    {
        pos[i] = beatsin16(i + 7, user_settings.led_offset, user_settings.led_end - 1);
        // On big strips dots move very fast, leaving gaps between their old and new
        // position, we fill these in with linear interpolated values
        // does not properly track the "bounces" at the end, but you barely notice that
        if (abs(pos[i] - oldPos[i]) <= MAX_POS_DIFF_TO_BRIDGE)
        {
            int smaller = pos[i] < oldPos[i] ? pos[i] : oldPos[i];
            int bigger =  pos[i] > oldPos[i] ? pos[i] : oldPos[i];
            for (int pdx = smaller+1; pdx < bigger; ++pdx)
            {
                float scale = 1.f - 20.f/256.f * (pdx - oldPos[i]) / (pos[i] - oldPos[i]);
                leds[pdx] |= CHSV(dothue * scale, 200 * scale, 255 * scale);
            }
        }
        leds[pos[i]] |= CHSV(dothue, 200, 255);
        dothue += 256 / DOT_COUNT;
    }
    memcpy(&oldPos, &pos, sizeof(oldPos));
}

void colorWipes()
{
    // fill led by led with one color after another
    static int mymode = 0;
    switch (mymode)
    {
    case 0:
        if (colorWipe(CRGB(255, 0, 0), 20))
        {
            mymode++;
        }
        break;
    case 1:
        if (colorWipe(CRGB(0, 255, 0), 20))
        {
            mymode++;
        }
        break;
    case 2:
        if (colorWipe(CRGB(0, 0, 255), 20))
        {
            mymode++;
        }
        break;
    case 3:
        if (colorWipe(CRGB(128, 128, 0), 20))
        {
            mymode++;
        }
        break;
    case 4:
        if (colorWipe(CRGB(0, 128, 128), 20))
        {
            mymode++;
        }
        break;
    case 5:
        if (colorWipe(CRGB(128, 0, 128), 20))
        {
            mymode++;
        }
        break;
    default:
        if (colorWipe(CRGB(85, 85, 85), 20))
        {
            mymode = 0;
        }
        break;
    }
}

int colorWipe(CRGB color, int wait)
{
    // fill led by led with one color
    static long rmm = 0;
    long cmm = millis() - rmm;
    uint32_t i = user_settings.led_offset + (cmm / wait);

    if (i >= user_settings.led_end)
    {
        rmm = millis();
        cmm = 0;
        if (i == (user_settings.led_end))
        {
            return 1;
        }
    }

    leds[i] = color;
    return 0;
}

// NOTE: This looks quite choppy with low brightness
void colorWheel()
{
    // cycle hue of each LED with offset between the LEDs
    long mm = millis();
    for (int pos = user_settings.led_offset; pos < user_settings.led_end; pos++)
    {
        leds[pos] = CHSV((mm / 10) - (pos * 255 / LED_LENGTH), 255, 255);
    }
}

// NOTE: This looks quite choppy with low brightness
void colorCircle()
{
    // cycle hue of whole stripe
    long mm = millis();
    for (int pos = user_settings.led_offset; pos < user_settings.led_end; pos++)
    {
        leds[pos] = CHSV(-mm / 50, 255, 255);
    }
}

void fadeToBlack(uint8_t byAmountEachFrame)
{
    fadeToBlackBy(leds + user_settings.led_offset, LED_LENGTH, byAmountEachFrame);
}