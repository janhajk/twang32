/*
    Local, per-installation settings. NOT in version control.

    Copy this file to  include/secrets.h  and fill it in. `secrets.h` is
    gitignored, so credentials never reach the public repository.

    Named "secrets" rather than "config" on purpose: src/config.h already
    exists and holds the hardware configuration. Two files called config.h
    would be a trap.
*/
#ifndef SECRETS_H
#define SECRETS_H

// ---------------------------------------------------------------------------
// WiFi
//
// Several networks, tried in the order listed. The kit runs on mobile access
// points that travel with it, so the SSID changes between events while the
// workshop network stays put — one entry per network you actually use beats
// reprovisioning three devices by hand at every venue.
//
// Leave the list with a single dummy entry if you only want the fallback AP.
// ---------------------------------------------------------------------------
#define WIFI_NETWORKS      \
    X("event-hotspot", "changeme") \
    X("werkstatt", "changeme")

// ---------------------------------------------------------------------------
// Backend
//
// API_BASE has no trailing slash. Paths are appended as "/api/...".
// ---------------------------------------------------------------------------
#define API_BASE "https://app.thingware.ch/twang"

// Shared secret that lets an unprovisioned device claim an identity. It is
// only ever used once per device: the reply carries a personal key that is
// stored in NVS from then on. Rotate it in the admin UI if it leaks.
#define PROVISION_TOKEN "changeme"

// Password for ArduinoOTA over the LAN, used while developing:
//   pio run -t upload --upload-protocol espota --upload-port <ip>
// This is the local convenience path. Fleet updates go through the backend.
#define OTA_LAN_PASSWORD "changeme"

#endif
