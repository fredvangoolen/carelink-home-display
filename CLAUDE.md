# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

A remote display for a Medtronic MiniMed 770G/780G insulin pump, for the
people who care for a Type-1 diabetes patient. Three programs, one project:

| Directory | What | Runs on | Licence |
|---|---|---|---|
| `firmware/minimed-coreink/` | the display: wakes every ~5 minutes, polls the proxy, redraws, deep-sleeps | M5Stack Core Ink (ESP32-PICO-D4, 1.54" e-paper, buzzer) | GPL-3.0 |
| `firmware/tools/nvs-slot-edit/` | one-shot sketch to change a WiFi slot in NVS | same board | GPL-3.0 |
| `desktop/` | the display's main screen in a window, for a caregiver's computer and for debugging; stdlib Python + Tk | Windows, macOS, Linux | GPL-3.0 |
| `proxy/` | holds the CareLink login, polls CareLink Cloud, re-serves the data on port 8081 | Raspberry Pi | AGPL-3.0 |
| `appliance/` | `provision.sh` and friends: turns a stock Pi into the proxy, with browser token renewal and the travel hotspots | Raspberry Pi | AGPL-3.0 |
| `docs/` | markdown, plus drawings in `img/` and the one-page handout in `handout/` (HTML source and its PDF) | | |

This is a medical-adjacent device in daily use. Timestamps, DST and alarm
handling have each produced silent, plausible-looking wrong answers before;
treat changes there as risky.

## The contract between the two halves

The firmware and the proxy share no code, only three URLs. A change to either
JSON shape is a breaking change for the other side — change both or neither.
`desktop/` and the proxy's own `/display` page read `nohistory` too:

- `GET /carelink/nohistory` — current pump data. If a field changes and the
  display breaks, look at the real response with `curl`; do not assume.
- `GET /carelink/trend` — the last 3 hours for the trend screen:
  `{"end": "<local timestamp>", "step": 300, "sg": [36 values, oldest first,
  0 = none]}`, or 404. `end` is local wall clock, read like
  `lastAlarm.dateTime`. All three frontends draw it the same way: the
  window ends now; the scale follows the readings but always shows 70 and
  180 (at least 60-200, within 40-400, ends rounded to 20; the top labelled
  only above 200); a finer line at 100, the pump's target, always; a gap
  breaks the line; hours only on the axis; a failed
  fetch keeps the previous trend.
- `GET /carelink/fallback` — `{"ssid": ..., "psk": ...}` for the Pi's travel
  hotspot, or 404. The display stores it in WiFi slot 2.

Three frontends read `nohistory` — the firmware, `desktop/minimed_desktop.py`
and `proxy/display/display.js` — and must apply the same rules: reading age
from epochs, the stale banner at 20 minutes, the link states, the alarm
window with `lastAlarm.dateTime` read as local time, low/high alarms
dropped once the reading recovers, and with no reading but the pump in
range of the phone, the pump's banner before "No sensor data"; DELIVERY_SUSPEND
reads "Pump suspended". Change them in all three or none.

Plan A / plan B (docs/travel.md) spans firmware slot 2, `appliance/travel-ap/`
and that endpoint. Document it as one feature, in `docs/travel.md`.

## Firmware

Start with `firmware/minimed-coreink/README.md` (build, measured timings,
hardware quirks) and `architecture.md` (entry point, file layout,
invariants).

```
FQBN=esp32:esp32:m5stack_coreink:PartitionScheme=huge_app
arduino-cli compile --fqbn "$FQBN" --upload -p <port> firmware/minimed-coreink
```

- `huge_app` is not optional: the default scheme leaves too little room.
- Config lives in NVS (`Preferences`, namespace `minimed`). An app upload does
  not touch NVS. To change a WiFi slot without redoing setup, use
  `firmware/tools/nvs-slot-edit/` (passwords via stdin, never printed), then
  reflash the real firmware.
- `main` is one cycle, not a loop: deep sleep restarts the sketch.
- A total WiFi failure skips the cycle. It must never fall into setup mode,
  which would strand a working monitor over a hiccup.
- The fault tables (`faults.cpp`) are generated data. Extend them; do not
  restructure them. After a change, run `python3 desktop/gen_faults.py`: it
  copies them to `desktop/faults.py` and `proxy/display/faults.js`, which are
  never edited by hand.
- ArduinoJson rejects input that looser parsers accept (an empty string from a
  proxy with a dead token, for one). Check what the proxy really sends.
- **Test builds** (`-DPLAN_A_RETRY_CYCLES=2`, `-DPORTAL_AP_TEST=1`,
  `LAYOUT_TEST`) must never be left on a board; see docs/testing.md.
- Power the board from a proper adapter. A weak USB port passes everything
  except the WiFi radio's inrush and then looks like a software hang
  (docs/lessons.md).
- There is no MicroPython build any more. Comments that mention "the
  MicroPython build" or "the Inkplate" are history, explaining why something
  is the way it is.

## Proxy

Everything builds on `proxy/carelink_client2.py` (`CareLinkClient`):

- `carelink_client2_proxy.py` — the daemon. Serves `GET /` (status),
  `/carelink`, `/carelink/nohistory`, `/carelink/trend`, `/carelink/fallback`, and `/display`
  (the live page in `proxy/display/`, from a fixed table of four files, with
  a strict CSP: it shows CareLink text on the origin that accepts the token
  POST, so data goes in as text only, never as HTML). When the token is
  invalid it serves a small web page to recover without SSH.
- `carelink_client2_cli.py` — one-shot or repeating download to a file.
- `carelink_carepartner_api_login.py` — browser login (Firefox, a human, a
  reCAPTCHA) that writes `logindata.json`. The token refreshes itself after
  that but expires if not refreshed for a week.

Read `docs/carelink-api.md` before changing auth or request logic: it encodes
hard-won findings about an undocumented API. The auth model differs by account
role (`PATIENT`, `PATIENT_OUS`, `CARE_PARTNER`); get it wrong and you get no
data rather than an error.

The login depends on selenium-wire, which is unmaintained; the version pins in
`appliance/provision.sh` are each there for a reason stated next to them.

## Appliance

`appliance/provision.sh` is idempotent and re-running it is the upgrade path.
It installs to fixed places on the Pi (`/usr/local/carelink`,
`/var/lib/carelink`, `/opt/carelink-*`, `/etc/carelink`); keep those stable so
an existing Pi upgrades in place. It never handles the VNC password and never
enables the display chain at boot — both deliberate, see
`appliance/README.md`, "Security model".

Switches: `TRAVEL_AP=0` leaves out the display's fallback hotspot;
`STAY_HOME=1` leaves out all three radio features (setup hotspot, WiFi
watchdog, fallback hotspot) for a Pi that never moves; `REBOOT_TIME=HH:MM`
moves the unattended-upgrade reboot from 14:00 (02:00 on the reference Pi,
which is also the house DNS server).

## Never commit

- `logindata.json` (live CareLink session) and `data-*.json` (patient data).
  Both are in `.gitignore`; do not remove those entries or force-add.
- Real names of people, households, WiFi networks or host names. The docs and
  comments use neutral ones (`carelink-01`, `seaview 12`, `office`, `Sam`).
  This repository is meant to be public.

## Verification

No test suite, linter or build step beyond the Arduino compile. See
docs/testing.md for what counts as verified and what is still untested.
