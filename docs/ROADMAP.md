# Roadmap — from a single game to an event platform

Where this is going: a fleet of TWANG32 controllers that join a normal WiFi network,
score properly, push results to a Thingware backend, get their configuration from the
cloud, and feed a public leaderboard that players can claim their scores on.

Every item below has an ID (`D`* = device firmware, `C`* = cloud). Effort is honest
effort for someone who knows the codebase: **S** = an evening, **M** = a day or two,
**L** = a project in its own right.

---

## 1. Architecture at a glance

```
   DEVICE (ESP32)                      CLOUD (AWS, eu-west-1)
   ─────────────                       ──────────────────────
   game loop                           Lambda  thingware-twang
   scoring          ──── plays ──────► ├─ /api/device/*   HMAC per device
   result queue     ◄─── config ─────┤ ├─ /api/public/*   no auth
   config cache                        ├─ /api/player/*   player token
   OTA              ◄─── firmware ───┤ └─ /api/admin/*    Cognito
                                       DynamoDB: devices, configs, games,
   printed QR sticker ──► player's     plays, players
                          phone
```

Frontend and backend follow `_corporate-design` (AWS Lambda + HTTP-API + DynamoDB,
no `node_modules`, CDN-only frontend, Cognito for staff). The device is **not** a
Cognito client — it authenticates with a per-device secret.

## 2. The setting this is built for

Settled, and the reason several decisions below look the way they do:

| | |
|---|---|
| **Fleet size** | **3 devices.** Small enough that remote config and an admin view are a convenience rather than a necessity — but they are being built anyway, because the value at this size is switching all three to a new game between events without opening anything or plugging in a cable. |
| **Network** | **Always available**, supplied by mobile access points that travel with the kit. Live scores are therefore realistic, not aspirational. |
| **Email** | **Optional, and motivated by a prize draw** rather than demanded. Name is required, email buys a ticket to the draw. |
| **Privacy** | Consent, purpose and retention are presented at registration, where the person actually is. The device never holds personal data (A2), so the whole obligation sits in one place in the cloud. |

## 3. Decisions taken up front

These constrain everything after them, so they are settled here rather than per item.

| # | Decision | Why |
|---|---|---|
| A1 | Players are **not** Cognito users | An event guest will not do a hosted-UI login. Players are lightweight records identified by a browser-held token. Cognito stays for staff. |
| A2 | The device never stores personal data | Names and emails live only in the cloud. The device knows a play ID, nothing else. Makes the data-protection story simple and keeps a stolen device worthless. |
| A3 | Score rules are **versioned** | A cloud config that changes scoring would silently corrupt a leaderboard. Each play stores its `scoringVersion`; each game pins one. |
| A4 | Nothing on the device blocks on the network | The game must play identically with the router unplugged. All cloud traffic is queued and handled off the game loop. |
| A5 | TLS with a pinned Amazon root CA | Not `setInsecure()`. Costs ~45 KB heap during the handshake, which we have. |
| A6 | Cloud config can never brick a device | Last-known-good plus a boot-loop counter that reverts to factory. Non-negotiable for a fleet. |

---

## PHASE 1 — Device foundations

### D1 · Over-the-air updates · **S**

**Goal.** Flash without opening the enclosure.

**Approach.** The board default partition table already has `app0`, `app1` and
`otadata`, each app slot 1.25 MB against our current 833 KB — so OTA needs no
repartitioning. Add `ArduinoOTA` for the workshop (push from PlatformIO over the LAN)
and, later, an HTTP pull from the backend so the fleet can be updated centrally.

**Decisions.** OTA listens only while the device is idle or in screensaver, never
mid-game. Password-protect the ArduinoOTA handler. Report the running firmware version
in every device heartbeat so the admin view can show what is out of date.

**Snag.** An interrupted OTA is safe (the bootloader keeps the old slot), but a
*successful* flash of a broken build is not. Pair this with D6's boot-loop counter.

**Depends on.** D2 for the pull variant; the push variant works as soon as the device
is on the LAN.

---

### D2 · Join the local WiFi · **M**

**Goal.** The device leaves AP-only mode, gets internet, and becomes reachable on the
network under a name.

**Approach.** Station mode with a fallback: if no credentials are stored, or the
network is unreachable for 30 s, fall back to the current `TWANG_AP` so the settings
page is always reachable. Add mDNS so it answers as `twang-<id>.local` instead of an
IP. Get the clock from NTP once associated — results need real timestamps and the
ESP32 has no RTC.

**Decisions.** Credentials go in NVS, never in the source. Provision them through the
existing AP page on first boot. Keep the AP running alongside station mode only while
unprovisioned — running both permanently costs stability and pins the radio channel.

**Store several networks, not one.** The kit runs on travelling mobile access points, so
the SSID changes between events, and the workshop network is a different one again. Keep
a short list (three or four) in NVS and try them in order at boot. Otherwise every event
starts with reprovisioning three devices by hand — which is exactly the kind of friction
that makes people leave the cloud features switched off.

**What a mobile hotspot implies.** Outbound to AWS works fine; inbound from the internet
does not, because carrier networks are behind CGNAT. That is not a problem: the device
only ever calls out. Within the hotspot's own LAN, mDNS works, so a phone on the same
hotspot reaches `twang-<id>.local` — which is the normal way anyone will open the
settings at an event. Keep the AP fallback regardless: devices will sometimes boot before
the hotspot is up, and they must not strand themselves when that happens.

**Snag.** ESP-NOW (a future two-player transport) shares the radio and the channel with
station mode. Not a problem now, but do not design the WiFi layer as if it owns the
radio forever.

---

### D3 · Real scoring · **M**

**Goal.** A score worth putting on a leaderboard.

**Today** it is one line: `score += lives * 10` per level cleared, level 0 excluded.
That caps a perfect run at roughly 600 points and rewards nothing but survival.

**Proposed model, `scoringVersion = 1`:**

| Component | Formula | Intent |
|---|---|---|
| Level clear | `+100` | Progress is the baseline |
| Speed bonus | `max(0, (par_ms − elapsed_ms) / 100)` | Rewards clearing fast, never negative |
| Life bonus | `+ lives × 10` | Keeps the existing incentive not to die |
| Boss kill | `+500` | The run's finale should dominate |

`par_ms` is per level and belongs in the level data, not in code — see D7. Start every
level at a flat 20 s par and tune from real play data once the backend is collecting it.

**Decisions.** The score must stay strip-length independent: the game already runs on a
1000-unit playfield, so keep all scoring in game units and never in LEDs. Report
`totalScore`, a per-level breakdown, and the run's **total elapsed milliseconds** — the
last one is what settles a tie.

**Ties.** Because time already feeds the score, exact ties at the top are unlikely rather
than routine. They are not impossible though: the speed bonus divides by 100, so it
resolves to 100 ms steps, and everyone who dies early scores the same. The rule costs
nothing and should exist before prize day rather than during it:

> Equal score → **shorter total run time wins.** Still equal → earlier submission wins.

That first tiebreak is a real measure of play rather than an arbitrary one, and the data
is already in the record.

**Snag.** Changing this later invalidates comparisons. That is precisely what A3's
`scoringVersion` is for — bump it, and let each game pin the version it accepts.

---

### D7 · Levels as data · **M**

Pulled forward from the previous roadmap because three later items need it: `par_ms`
(D3), remote level sets (D6) and any second game mode. Every level is already just a
short list of spawn calls; turning the `switch` into an array of records makes level
sets loadable, editable and shippable from the cloud.

---

## PHASE 2 — Talking to the cloud

### D4 · Result queue and non-blocking sync · **M**

**Goal.** Results reach the cloud eventually, and the game never waits for them.

**Approach.** A ring buffer of finished plays in SPIFFS (1.4 MB available — thousands
of records). The game loop only appends. A separate FreeRTOS task on the other core
drains the queue: POST, on `2xx` drop the record, otherwise back off and retry. A play
is only removed once the server has acknowledged it.

**Decisions.** Every play gets a device-generated **UUID** at creation, and the upload
is **idempotent** on that ID, so a retry after a lost response cannot double-count.
Records carry `playedAt` from NTP when available; if the clock was never set, send
`uptimeMs` instead and let the backend derive a timestamp from receipt.

**Live scores** are a *separate*, fire-and-forget channel: while a game is running,
POST the current level and score every two seconds to `/api/device/live`. Never queued,
never retried — a stale live score is worse than none. This is what makes a big screen
at an event possible.

**Still needed even with reliable WiFi.** Hotspots drop, venues are radio-hostile, and
a device will sometimes boot before the access point does. The queue is what turns those
from lost results into delayed ones. What good connectivity buys is the *live* channel
being worth building.

**Snag.** Do not let the queue task allocate on the game loop's core; FastLED's RMT
output is timing-sensitive. Pin the network task to core 0 and leave core 1 alone.

---

### D5 · Game ID · **S**

**Goal.** An event organiser starts a "game", and every device that day writes into it.

**Approach.** The device stores a `gameId` in NVS and stamps it on every play. It can
be set three ways: locally on the settings page, remotely through the cloud config
(D6), or by the admin platform (C5). When unset, plays land in the device's default
open game so nothing is ever lost.

**Decisions.** The device does not validate the game — it just reports what it was
told. The backend decides whether the game is still open and flags late arrivals
rather than dropping them.

---

### D6 · Configuration from the cloud · **M**

**Goal.** Change one config, and every device follows at its next boot.

**At three devices** this is not about scale. It is about walking into a venue and
switching all of them to the day's game and settings without a cable, an enclosure screw
or a laptop. That is worth the build even for a small fleet — but it does mean the
rollback machinery below is proportionally more important than the feature itself,
because a bad config takes out the whole event rather than a fraction of it.

**Approach.** Two modes in NVS: `factory` (compiled-in defaults, the safe harbour) and
`cloud` plus a `configId`. In cloud mode the device fetches
`/api/device/config?configId=…` at boot with `If-None-Match` carrying the cached ETag —
`304` means keep what we have, `200` means apply and persist. No internet means the
cached config is used. No cache either means factory.

**Decisions.** This is the item that can take a whole fleet down, so it gets two
safety nets:

- **Last-known-good.** A config is only promoted to "good" after the device has run for
  60 s without a reset. A boot counter in NVS reverts to factory after three failed
  boots.
- **A physical escape hatch.** Hold the spring tilted through boot to force factory
  config, whatever the cloud says. If a bad config ever ships, this is the difference
  between a five-minute recovery and collecting fifty enclosures.

**Snag.** Do not let cloud config change scoring parameters without bumping
`scoringVersion` — see A3.

---

### D8 · Vibration in the base · **S** (firmware) + hardware

**Goal.** A hit you feel.

**Part.** Shop of Things [Vibrationsmodul 5V](https://shopofthings.ch/shop/prototyping/aktoren/vibrationsmodul-5v/),
CHF 6.90. Ø10 mm coin ERM, 3.0–5.3 V, ≤60 mA running and ≤90 mA at startup, with a
**MOSFET already on the board** — VCC / GND / IN straight to a GPIO, no external
driver. 21 × 23 × 8 mm, two M3 holes on a 15 mm pitch. Takes PWM for intensity.

**Approach.** Mount it in the **base**, not in the knob, on **GPIO 27** (free, no
strapping function), driven through LEDC so strength is adjustable and can be turned
down or off in the settings.

Events worth a pulse: attack connects, player dies, level cleared, boss hit.

**Why the base and not the knob.** Two reasons, and the first outweighs the haptics
argument for putting it where the hand is:

- **No wires along the spring.** The spring *is* the input device. It already carries
  four wires for the MPU6050; three more stiffen it, change its damping and will fatigue
  after a few thousand twangs. The knob is the worst place in the device to add hardware.
- **The spring is a mechanical isolator.** A coin ERM at 5 V runs around 150–200 Hz. The
  spring-plus-knob assembly resonates somewhere near 5–20 Hz — which is exactly the band
  the game samples. Well above resonance, transmissibility falls off as roughly 1/f², so
  the sensor at the top sees almost nothing of the motor at the bottom.

**Two consequences of that reasoning.**

- **Do not soft-start the motor.** Spinning up sweeps through every frequency below the
  running speed, including the spring's resonance. Full PWM immediately gets through that
  band in a few milliseconds; a gentle ramp would sit in it and excite the spring.
- **This conflicts with the optional second MPU6050.** The reference gyro at 0x69 lives
  in the base and exists to measure base motion so it can be subtracted from the knob
  sensor. Putting the motor in the base writes commutator noise straight into that
  differential. In this layout, haptics and the dual-sensor mode are mutually exclusive.

**Practical points.** Mount it rigidly — screwed to the shell or glued; foam-mounting
kills the effect that makes it worth fitting. That means two M3 bosses on a 15 mm pitch
in `TWANG32_CHASSIS-mod`, i.e. a CAD change. Feed 5 V from the strip supply rather than
through the ESP32, put a 100 µF cap at the module, and keep the motor leads away from
the I2C pair.

**Test before the CAD work.** Tape the module into the base, fire pulses, and watch the
raw values with `-DJOYSTICK_DEBUG`. Ten minutes tells you whether gating the wobble
sampling during a pulse is needed at all. Going by the numbers above, it probably is not.

---

## PHASE 3 — Who played?

### C1 · Player identity and score claiming · **M**

This is the part you asked me to sanity-check. **The concept is sound** — a claim-code
flow that keeps personal data off the device is the right shape. Three changes I would
make:

#### Change 1 — no display in version one

A QR code needs a two-dimensional display, and the device has none. Adding an OLED means
new hardware, a new enclosure cutout and QR rendering on the ESP32.

**Instead: a printed QR sticker on the enclosure**, encoding a fixed
`…/twang/p/<deviceId>`. After a run the player scans it, and the page asks the backend
*"what was the last unclaimed play on this device?"* and shows **"4,280 points, 14
seconds ago — was that you?"** before claiming.

This ships now, with no hardware and no per-play rendering. The risk is two people
playing back-to-back and the wrong score being claimed; a short claim window (90 s), the
confirmation screen showing score and time, and one-shot claiming make that acceptable
for a queue where one person plays at a time.

A display stays on the table as a later upgrade (an SSD1306 at 128×64 fits a version-3
QR at two pixels per module) for the case where precision turns out to matter.

#### Change 2 — email optional, and paid for with a prize

A leaderboard needs a display name. Nothing else. Requiring an email turns a game into a
data-collection exercise, raises the revDSG/GDPR bar, and costs conversions right at the
machine, where the player has a queue behind them.

Ask for the name; offer the email as an **entry to the prize draw**. That is an honest
exchange — the person gets something for it, and the ones who decline still appear on the
board. Present consent, purpose and retention period at that point, per
`_corporate-design/company-and-legal.md`.

One consequence worth handling: a winner who gave no email cannot be contacted. Their
permalink is the fallback — show the win there, so a player who bookmarked their page
finds out without you holding their address.

#### Change 3 — the permalink *is* the identity

You want a secret permalink for players anyway. Make it the same token as the
registration: `…/twang/me/<playerToken>` is both "your scores, any time" and the thing
localStorage holds for auto-claiming. One mechanism, two features, and a player whose
browser data got wiped can recover by bookmark instead of re-registering.

If they gave an email, a magic link recovers the token as well.

#### The flow

```
1.  Device finishes a run.
    POST /api/device/plays  { playId, deviceId, gameId, score, breakdown, playedAt }
    → play stored, unclaimed

2.  Player scans the sticker → …/twang/p/<deviceId>
    Page has a token in localStorage?
      yes → auto-claim, show the score, done
      no  → ask for a display name (email optional), create player, claim

3.  POST /api/player/claim { deviceId, playerToken }
    → server takes the newest unclaimed play on that device within 90 s,
      sets playerId, returns the score

4.  Next time: step 2 short-circuits. One scan, no typing.
```

**Race handling.** The claim can arrive before the play does — the device may have been
offline. Model a play as a record that either side may create: whichever arrives first
writes it, the second fills in its half. Both operations idempotent on `playId`.

**Abuse.** The claim is scoped to a device and a 90-second window, so the worst case at
an event is claiming the score of the person in front of you. Sufficient for a
trade-fair leaderboard; if it ever needs to be airtight, that is what the display
upgrade buys.

#### Retention

Two different pieces of data with two different lifetimes, because they serve two
different purposes:

| Data | Kept | Why that long |
|---|---|---|
| **Email address** | **90 days after the event ends** | Long enough to run the draw, reach the winner, and re-draw if they never answer. Nothing beyond that is a purpose, so nothing beyond that is kept. |
| **Display name + score** | **12 months** | Keeps a bookmarked permalink meaningful, and lets you refer back to last year's event. Drop to 6 if you would rather hold less. |

**Enforce it in the infrastructure, not in a calendar reminder.** Give both record types a
DynamoDB **TTL attribute** and let AWS do the deleting. That means the promise made at
registration is kept even if nobody remembers to keep it.

For that to work the email has to be separable from the player record, since TTL expires
whole items and not single attributes. So the email lives in its own
`thingware-twang-contacts` item keyed by `playerId`, with its own 90-day TTL, while the
player record carries the 12-month one. A player whose contact record has expired still
has a working permalink and still appears on the board — they simply can no longer be
mailed, which is exactly the intended end state.

State both numbers in the registration text. They are short enough to be credible and
specific enough to be worth something.

---

## PHASE 4 — The platform

### C2 · Backend skeleton · **M**

Per `_corporate-design/aws-infrastructure.md`, no deviations:

- One Lambda `thingware-twang` on the newest GA Node runtime, serving both the JSON API
  and the static SPA
- API Gateway **HTTP-API v2**, single `$default` route, mapped to
  **`app.thingware.ch/twang`** (new mapping key; add the callback URLs to the standard
  Cognito client)
- DynamoDB on-demand, access wrapped in `utils/dynamodb/*.mjs`
- No `node_modules`, vanilla SigV4 and JWKS verification, frontend from CDN
  (Bootstrap 5.3.3, Font Awesome 6.6.0, jQuery 3.7.1, Select2)
- Base-path-agnostic frontend with the mandatory `<base>` guard, i18n with English as
  master, `changelog.txt`, version auto-reload

**Tables** (naming per convention `thingware-twang-<purpose>`):

| Table | Key | Holds |
|---|---|---|
| `…-devices` | `deviceId` | name, secret hash, last seen, firmware, configId, gameId |
| `…-configs` | `configId` | version, ETag, payload, updatedAt |
| `…-games` | `gameId` | name, window, active, public slug, scoringVersion |
| `…-plays` | `playId` | deviceId, gameId, score, breakdown, playedAt, playerId |
| `…-players` | `playerId` | display name, token hash, 12-month TTL |
| `…-contacts` | `playerId` | email only, 90-day TTL — separate so it expires on its own |

`…-plays` needs two indexes: `gameId + score` for the leaderboard, and
`deviceId + playedAt` for the claim lookup.

**Four auth zones in one router:** `/api/device/*` by device HMAC, `/api/player/*` by
player token, `/api/public/*` open, `/api/admin/*` by Cognito with group `twang:admin`.

---

### C3 · Device API and device authentication · **M**

Devices cannot do Cognito. Each gets a secret at provisioning; requests carry an HMAC
over body plus timestamp plus a monotonic counter, which gives both authenticity and
replay protection. A device that fails authentication is logged, not silently dropped —
that log is how you notice a misconfigured unit at an event.

Endpoints: `POST /plays`, `POST /live`, `GET /config`, `POST /heartbeat`,
`GET /firmware`.

---

### C4 · Public leaderboard · **M**

- `…/twang/board/<gameSlug>` — public, no login, live-polling top N
- The slug is unguessable, so the link is shareable without being enumerable
- `…/twang/me/<playerToken>` — a player's own runs, their rank, their best
- Built for a projector as well as a phone: a big-screen mode with the live channel
  from D4 showing the run in progress

**Per event, no all-time board.** Every ranking is scoped to one game, which keeps the
leaderboard query a single `gameId + score` lookup and means a whole event's data can be
retired in one operation when its retention runs out. A returning player's permalink
shows their runs grouped by event, but there is no cross-event ranking to defend or to
explain.

**Freeze on `playedAt`, not on arrival.** A device that was offline may upload a run
minutes or hours after the event closed, and that run was still played inside the window
— it must count. So the window is judged by when the game was played, while a run whose
`playedAt` falls outside it is stored but excluded. Mark the board **final** once the
window has closed and every device's queue has drained, so a published ranking cannot
quietly change afterwards.

---

### C5 · Admin platform · **M**

- **Devices:** every unit with last seen, firmware, current config and game, plays
  today; assign config and game individually or by group
- **Config templates:** author a config, version it, roll it out; see which devices
  have picked it up and which are lagging
- **Games:** create, open and close, set the window, get the public slug
- **Players:** list and delete, which is the GDPR deletion path
- Cognito, RBAC group `twang:admin`, group switcher and sidebar footer per the
  corporate design

---

## Suggested order

| # | Item | Size | Why here |
|---|---|---|---|
| 1 | D1 OTA | S | Everything after this gets faster to test |
| 2 | D2 WiFi station | M | Prerequisite for all cloud work |
| 3 | C2 Backend skeleton | M | Something for the device to talk to |
| 4 | D3 Scoring | M | Results are worthless until the score is |
| 5 | D4 Queue + sync | M | First real data flowing |
| 6 | C3 Device API | M | Pairs with D4, build them together |
| 7 | C1 Claiming + C4 board | M | The moment it becomes an event product |
| 8 | D5 Game ID | S | Small once the plumbing exists |
| 9 | D6 Cloud config | M | Only pays off with more than a couple of devices |
| 10 | C5 Admin platform | M | Needs everything above to have something to manage |
| 11 | D8 Vibration (base) | S | Independent of all of it; slot it anywhere |
| 12 | D7 Levels as data | M | Do it before any second game mode |

## Still open

- **What is the prize?** Not a technical question, but it decides the registration copy
  and how hard the draw has to be to argue with.
- **Does an event need an on-site fallback view** if the venue's uplink dies mid-day —
  the devices keep queueing, but the projector goes blank. A cached last-known board
  served from a laptop would cover it, and is only worth building if that scenario feels
  real.
