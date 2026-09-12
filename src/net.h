/*
    Network layer: WiFi station mode, self-provisioning, cloud config and
    pull OTA. Modelled on thingware-flux so both fleets behave the same way.

    The one thing that differs from Flux, and the reason this file exists at
    all rather than living in the main loop: TWANG renders a light strip on a
    frame clock. An HTTP request blocks for seconds, and an OTA download for
    far longer. So nothing in here runs while a game is in progress — the main
    loop calls net_idle_tick() only when the device is sitting in its
    screensaver. A frozen screensaver is invisible; a frozen game is not.

    Flow, same as Flux:
      no key in NVS  ->  POST /api/provision  { token, chipId, board, fw }
                         reply carries "<deviceId>.<secret>", stored in NVS
      periodically   ->  GET  /api/device-config?fw=..&board=..   x-api-key
                         reply may carry { update: { version, url } }
      update offered ->  HTTPUpdate from a presigned S3 URL, then reboot
      401 anywhere   ->  key was rotated or revoked: provision again
*/
#ifndef NET_H
#define NET_H

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#include "secrets.h"

// Bump this for every build you intend to roll out. The server compares it to
// the version it has on file as an EXACT STRING, so "1.0.0" and "1.0" are two
// different firmwares and a rollback is just re-publishing the older string.
#define FW_VERSION "1.0.6"

#ifndef BOARD_TAG
#define BOARD_TAG "esp32dev"
#endif

// How often to check in. Long enough that the fleet is not chatty, short
// enough that pushing an update at an event does not mean waiting an hour.
#define CONFIG_POLL_MS (10UL * 60UL * 1000UL)
// An unprovisioned device polls quickly: it is waiting to be let in.
#define PROVISION_RETRY_MS (30UL * 1000UL)
// Give up on the WiFi rather than stall the device forever.
#define WIFI_CONNECT_MS 12000

static const char AMAZON_ROOT_CA1[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)CERT";

// Definiert in TWANG32.ino, weiter unten in derselben Uebersetzungseinheit.
void applyStripMode();
void applyPowerLimit();

static WiFiMulti wifiMulti;
static Preferences netPrefs;

static String apiKey;        // "<deviceId>.<secret>", persisted in NVS
static String netDeviceId;   // the part before the dot
static String netDeviceName;
static bool netPending = true;   // true until an admin activates the device
static bool netHaveStation = false;
static unsigned long lastPollMs = 0;
static uint32_t appliedCfgRev = 0;   // zuletzt uebernommene Konfigurationsrevision

/* ------------------------------------------------------- Partien melden */

// Vorwaertsdeklaration: die Definition steht weiter unten bei der
// Provisionierung, das Melden braucht sie aber schon hier.
static int netAuthedRequest(const char *method, const String &path,
                            const String &body, String &reply);

// Kleine Warteschlange im RAM. Ein fehlgeschlagener Upload darf eine Partie
// nicht verlieren, aber sie muss auch nicht ueber einen Neustart hinaus
// ueberleben - dafuer waere sie den Flash-Verschleiss nicht wert.
#define PLAY_QUEUE_LEN 8

struct PendingPlay
{
    char id[40];
    uint32_t score;
    uint16_t levels;
    uint32_t durationMs;
};

static PendingPlay playQueue[PLAY_QUEUE_LEN];
static uint8_t playQueueCount = 0;

/** Zufaellige, praktisch eindeutige Kennung - der Server macht sie idempotent. */
static void netNewPlayId(char *out)
{
    const char *hex = "0123456789abcdef";
    // Muster wie eine UUID, damit die Serverpruefung sie annimmt
    const char *form = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";
    for (int i = 0; form[i]; i++)
        out[i] = form[i] == '-' ? '-' : hex[esp_random() & 0x0F];
    out[36] = 0;
}

/** @returns true, wenn der Server die Partie angenommen hat. */
static bool netSendPlay(const PendingPlay &p)
{
    if (!netHaveStation || apiKey.length() == 0)
        return false;

    JsonDocument doc;
    doc["playId"] = p.id;
    doc["score"] = p.score;
    doc["levelsCleared"] = p.levels;
    doc["durationMs"] = p.durationMs;
    doc["scoringVersion"] = 1;
    String body;
    serializeJson(doc, body);

    String reply;
    return netAuthedRequest("POST", "/api/plays", body, reply) == 200;
}

/** Nimmt eine fertige Partie an und versucht sie sofort zu senden. */
void net_report_play(const char *id, uint32_t score, uint16_t levels, uint32_t durationMs)
{
    PendingPlay p;
    strncpy(p.id, id, sizeof(p.id) - 1);
    p.id[sizeof(p.id) - 1] = 0;
    p.score = score;
    p.levels = levels;
    p.durationMs = durationMs;

    if (netSendPlay(p))
    {
        Serial.printf("[play] %lu Punkte gemeldet\r\n", (unsigned long)score);
        return;
    }

    if (playQueueCount < PLAY_QUEUE_LEN)
    {
        playQueue[playQueueCount++] = p;
        Serial.printf("[play] Upload fehlgeschlagen, %d in der Warteschlange\r\n", playQueueCount);
    }
    else
    {
        // Voll: die aelteste faellt raus. Eine neue Partie ist mehr wert als
        // eine alte, die seit acht Versuchen nicht durchgeht.
        memmove(&playQueue[0], &playQueue[1], sizeof(PendingPlay) * (PLAY_QUEUE_LEN - 1));
        playQueue[PLAY_QUEUE_LEN - 1] = p;
        Serial.println("[play] Warteschlange voll - aelteste Partie verworfen");
    }
}

/** Arbeitet die Warteschlange ab, eine Partie pro Aufruf. */
static void netFlushPlays()
{
    if (playQueueCount == 0)
        return;
    if (!netSendPlay(playQueue[0]))
        return;
    Serial.printf("[play] nachgemeldet, noch %d offen\r\n", playQueueCount - 1);
    memmove(&playQueue[0], &playQueue[1], sizeof(PendingPlay) * (playQueueCount - 1));
    playQueueCount--;
}

// The eFuse MAC, which is unique per chip and survives a reflash. This is how
// a device proves it is the same one across provisioning rounds.
static String netChipId()
{
    uint64_t mac = ESP.getEfuseMac();
    char hex[17];
    snprintf(hex, sizeof hex, "%012llx", (unsigned long long)(mac & 0xFFFFFFFFFFFFULL));
    return String(hex);
}

static String netHostname()
{
    // Die vorderen sechs Zeichen, nicht die hinteren: ESP.getEfuseMac() liefert
    // die MAC byteweise umgedreht, also stehen hinten die ersten MAC-Bytes -
    // und das ist Espressifs Herstellerkennung, auf jedem ESP32 dieselbe. Mit
    // substring(6) hiessen alle Geraete gleich, und zwei im selben Netz
    // stritten sich um denselben mDNS-Namen. An der Hardware aufgefallen, als
    // ein zweites Board ebenfalls als twang-d33fb0 erschien.
    return String("twang-") + netChipId().substring(0, 6);
}

/* --------------------------------------------------------------- transport */

static bool netEnsureWifi()
{
    if (WiFi.status() == WL_CONNECTED)
        return true;
    unsigned long start = millis();
    while (wifiMulti.run(4000) != WL_CONNECTED && millis() - start < WIFI_CONNECT_MS)
        delay(150);
    return WiFi.status() == WL_CONNECTED;
}

/** @returns HTTP status, or a negative value on a transport failure. */
static int netRequest(const char *method, const String &path, const String &body,
                      String &reply, bool withKey = true)
{
    if (!netEnsureWifi())
        return -1;

    WiFiClientSecure client;
    client.setCACert(AMAZON_ROOT_CA1);

    HTTPClient http;
    if (!http.begin(client, String(API_BASE) + path))
        return -1;
    if (withKey && apiKey.length())
        http.addHeader("x-api-key", apiKey);
    if (body.length())
        http.addHeader("content-type", "application/json");
    http.setTimeout(10000);

    int status = body.length() ? http.sendRequest(method, body) : http.sendRequest(method);
    if (status > 0)
        reply = http.getString();
    http.end();

    if (status != 200)
        Serial.printf("[http] %s %s -> %d\r\n", method, path.c_str(), status);
    return status;
}

/* ------------------------------------------------------------ provisioning */

static bool netProvision()
{
    JsonDocument doc;
    doc["token"] = PROVISION_TOKEN;
    doc["chipId"] = netChipId();
    doc["board"] = BOARD_TAG;
    doc["fw"] = FW_VERSION;
    String body;
    serializeJson(doc, body);

    String reply;
    if (netRequest("POST", "/api/provision", body, reply, false) != 200)
        return false;

    JsonDocument r;
    if (deserializeJson(r, reply))
        return false;
    const char *key = r["apiKey"];
    if (!key)
        return false;

    apiKey = key;
    netDeviceId = apiKey.substring(0, apiKey.indexOf('.'));
    netPrefs.putString("apiKey", apiKey);
    netPending = r["pending"] | false;
    if (r["name"].is<const char *>())
        netDeviceName = r["name"].as<const char *>();

    Serial.printf("[prov] provisioned as %s%s\r\n",
                  netDeviceId.c_str(), netPending ? " (pending activation)" : "");
    return true;
}

/**
 * A 401 means the key was rotated or revoked on the server. Re-provision once,
 * then retry — but no more often than PROVISION_RETRY_MS, so a genuinely
 * rejected device does not hammer the API.
 */
static int netAuthedRequest(const char *method, const String &path,
                            const String &body, String &reply)
{
    int status = netRequest(method, path, body, reply);
    if (status == 401)
    {
        static unsigned long lastTry = 0;
        if (millis() - lastTry > PROVISION_RETRY_MS)
        {
            lastTry = millis();
            Serial.println("[auth] key rejected, provisioning again");
            apiKey = "";
            netPrefs.remove("apiKey");
            if (netProvision())
                status = netRequest(method, path, body, reply);
        }
    }
    return status;
}

/* ---------------------------------------------------------------- pull OTA */

static void netOtaPull(const char *url, const char *version)
{
    Serial.printf("[ota] %s -> %s, downloading\r\n", FW_VERSION, version);

    // No BLE here, unlike Flux, so modem sleep can go off for the transfer
    // without any coexistence risk. Leaving it on turns a ~1 MB download into
    // a string of read timeouts.
    WiFi.setSleep(false);

    WiFiClientSecure client;
    client.setCACert(AMAZON_ROOT_CA1);

    HTTPUpdate updater(20000);
    updater.rebootOnUpdate(true);   // on success this call never returns
    updater.update(client, url);

    Serial.printf("[ota] failed: %d %s\r\n",
                  updater.getLastError(), updater.getLastErrorString().c_str());
    WiFi.setSleep(true);
}

/* -------------------------------------------------- Fernkonfiguration */

/**
 * Setzt ein Feld aus der Antwort, auf die erlaubten Grenzen beschnitten.
 * @returns true, wenn sich der Wert tatsaechlich geaendert hat.
 */
template <typename T>
static bool netSetField(T &ziel, JsonVariantConst v, long lo, long hi)
{
    if (v.isNull())
        return false;                       // Feld nicht gesetzt: unangetastet lassen
    long n = v.as<long>();
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    if ((long)ziel == n)
        return false;
    ziel = (T)n;
    return true;
}

/**
 * Uebernimmt die Einstellungen aus der Server-Antwort.
 *
 * Der Revisionszaehler ist der Grund, warum das hier ueberhaupt gefahrlos ist:
 * ohne ihn wuerde jeder Poll das EEPROM neu beschreiben - alle paar Minuten,
 * dauerhaft, auf jedem Geraet der Flotte. Verglichen wird gegen den im NVS
 * gemerkten Wert, damit ein Neustart die Runde nicht wiederholt.
 */
static void netApplySettings(JsonDocument &doc)
{
    uint32_t rev = doc["settingsRev"] | 0;
    if (rev == 0 || rev == appliedCfgRev)
        return;

    JsonObjectConst s = doc["settings"];
    if (!s.isNull())
    {
        bool geaendert = false;
        geaendert |= netSetField(user_settings.led_end,                   s["led_end"],                   MIN_LEDS, MAX_LEDS);
        geaendert |= netSetField(user_settings.led_offset,                s["led_offset"],                0, MAX_LEDS - MIN_LEDS);
        geaendert |= netSetField(user_settings.led_brightness,            s["led_brightness"],            MIN_BRIGHTNESS, MAX_BRIGHTNESS);
        geaendert |= netSetField(user_settings.led_brightnessScreensaver, s["led_brightnessScreensaver"], MIN_BRIGHTNESS, MAX_BRIGHTNESS);
        geaendert |= netSetField(user_settings.strip_mode,                s["strip_mode"],                MIN_STRIP_MODE, MAX_STRIP_MODE);
        geaendert |= netSetField(user_settings.audio_volume,              s["audio_volume"],              MIN_VOLUME, MAX_VOLUME);
        geaendert |= netSetField(user_settings.joystick_deadzone,         s["joystick_deadzone"],         MIN_JOYSTICK_DEADZONE, MAX_JOYSTICK_DEADZONE);
        geaendert |= netSetField(user_settings.attack_threshold,          s["attack_threshold"],          MIN_ATTACK_THRESHOLD, MAX_ATTACK_THRESHOLD);
        geaendert |= netSetField(user_settings.lives_per_level,           s["lives_per_level"],           MIN_LIVES_PER_LEVEL, MAX_LIVES_PER_LEVEL);
        geaendert |= netSetField(user_settings.power_limit_ma,            s["power_limit_ma"],            MIN_POWER_LIMIT_MA, MAX_POWER_LIMIT_MA);

        // Der Versatz darf das Stripende nicht ueberholen - sonst ergaebe
        // LED_LENGTH eine negative Spielfeldlaenge.
        if (user_settings.led_offset > user_settings.led_end - MIN_LEDS)
        {
            user_settings.led_offset = user_settings.led_end - MIN_LEDS;
            geaendert = true;
        }

        if (geaendert)
        {
            settings_eeprom_write();
            applyStripMode();
            applyPowerLimit();
            sound_master_volume(user_settings.audio_volume);
            FastLED.setBrightness(user_settings.led_brightness);
            Serial.printf("[config] uebernommen: %d LEDs, Helligkeit %d, Modus %d, Budget %d mA\r\n",
                          user_settings.led_end, user_settings.led_brightness,
                          user_settings.strip_mode, user_settings.power_limit_ma);
        }
        else
        {
            Serial.println("[config] neue Revision, aber identische Werte - EEPROM unberuehrt");
        }
    }

    appliedCfgRev = rev;
    netPrefs.putUInt("cfgRev", rev);
}

/* ------------------------------------------------------------------ config */

static void netFetchConfig()
{
    String reply;
    String path = String("/api/device-config?fw=") + FW_VERSION + "&board=" + BOARD_TAG;
    if (netAuthedRequest("GET", path, "", reply) != 200)
        return;

    JsonDocument doc;
    if (deserializeJson(doc, reply))
        return;

    netPending = doc["pending"] | false;
    if (doc["name"].is<const char *>())
        netDeviceName = doc["name"].as<const char *>();

    const char *ev = doc["eventId"];
    Serial.printf("[config] %s, Anlass %s\r\n",
                  netPending ? "PENDING - activate it in the admin UI" : "aktiv",
                  ev ? ev : "(keiner - Punkte werden verworfen)");

    // Einstellungen vor dem OTA: sollte der Download scheitern, laeuft das
    // Geraet wenigstens mit der richtigen Konfiguration weiter.
    netApplySettings(doc);

    // Last, because a successful update reboots and never comes back.
    if (doc["update"]["url"].is<const char *>())
        netOtaPull(doc["update"]["url"], doc["update"]["version"] | "?");
}

/* -------------------------------------------------------------------- API */

/**
 * Brings up WiFi and restores the stored key. Returns true when a station
 * connection was established, which tells the caller whether the fallback
 * access point is still needed.
 */
static bool net_begin()
{
    netPrefs.begin("twangnet", false);
    apiKey = netPrefs.getString("apiKey", "");
    appliedCfgRev = netPrefs.getUInt("cfgRev", 0);
    if (apiKey.length())
        netDeviceId = apiKey.substring(0, apiKey.indexOf('.'));

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(netHostname().c_str());
#define X(ssid, pass) wifiMulti.addAP(ssid, pass);
    WIFI_NETWORKS
#undef X

    Serial.printf("\r\nfw %s, board %s, chip %s\r\n", FW_VERSION, BOARD_TAG, netChipId().c_str());
    netHaveStation = netEnsureWifi();

    if (netHaveStation)
    {
        Serial.printf("[wifi] %s as %s (%s)\r\n",
                      WiFi.SSID().c_str(), netHostname().c_str(),
                      WiFi.localIP().toString().c_str());
        if (MDNS.begin(netHostname().c_str()))
            Serial.printf("[wifi] also reachable at http://%s.local/\r\n", netHostname().c_str());

        // Local convenience path for development. Fleet updates go through the
        // backend; this one only needs the device and the laptop on one LAN.
        ArduinoOTA.setHostname(netHostname().c_str());
        ArduinoOTA.setPassword(OTA_LAN_PASSWORD);
        ArduinoOTA.begin();

        // Anmelden und einmal Konfiguration holen, solange noch niemand spielt.
        //
        // Der Leerlauf-Poll allein genuegt nicht: "Leerlauf" heisst Screensaver,
        // und dorthin kommt das Geraet nur, wenn der Sensor laenger als TIMEOUT
        // still steht. Ein schraeg liegender MPU6050 meldet dauerhaft Neigung,
        // also haelt das Spiel das fuer einen spielenden Menschen und der
        // Screensaver kommt nie -- an der Hardware genau so beobachtet. Am Event
        // waere es dasselbe, nur mit echten Spielern.
        //
        // Hier zu blockieren ist unbedenklich: setup() laeuft vor dem ersten
        // Frame. Damit genuegt ein Neustart, um ein Update zu ziehen, auch wenn
        // das Geraet nie zur Ruhe kommt.
        lastPollMs = millis();
        if (apiKey.length() == 0)
            netProvision();
        if (apiKey.length())
            netFetchConfig();
        netFlushPlays();
    }
    else
    {
        Serial.println("[wifi] no network - falling back to the access point");
    }
    return netHaveStation;
}

/**
 * Call this ONLY when the game is idle. Everything in here can block for
 * seconds, and the OTA download for a lot longer than that.
 */
static void net_idle_tick()
{
    if (!netHaveStation)
        return;

    ArduinoOTA.handle();
    netFlushPlays();   // liegengebliebene Partien zuerst

    unsigned long due = netPending || apiKey.length() == 0 ? PROVISION_RETRY_MS : CONFIG_POLL_MS;
    if (millis() - lastPollMs < due)
        return;
    lastPollMs = millis();

    if (apiKey.length() == 0)
    {
        netProvision();
        return;
    }
    netFetchConfig();
}

#endif
