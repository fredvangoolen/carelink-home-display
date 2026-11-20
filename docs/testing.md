# Testing

There is no automated test suite on either side. The firmware is verified on
the board, the appliance on a Pi. This page says how, and what has not been
done yet.

## Firmware test builds

Three compile-time switches exist only for testing. Set them with
`--build-property "compiler.cpp.extra_flags=..."` on the `arduino-cli compile`
command line; `LAYOUT_TEST` is a `#define` in the sketch.

| Switch | What it does |
|---|---|
| `-DPLAN_A_RETRY_CYCLES=2` | on plan B, try plan A again every 2 cycles (10 minutes) instead of every 12 (one hour) |
| `-DPORTAL_AP_TEST=1` | open the setup portal on every boot and store nothing |
| `LAYOUT_TEST 1` | compose every screen once with synthetic data, including an alarm |

**A test build must never be left on a board.** `PORTAL_AP_TEST` in particular
keeps the board out of monitoring altogether: it shows a setup page instead of
a glucose reading. After testing, flash the normal build and watch one full
cycle.

`PORTAL_AP_TEST` announces itself on the serial console at boot;
`PLAN_A_RETRY_CYCLES` does not. When in doubt about what a board runs,
reflash the normal build.

## What "verified" means here

For anything touching the display, WiFi, the proxy lookup or deep sleep: flash
it, and watch at least one full wake cycle on the serial console and on the
panel — several if the change is about timing, because some failures only show
up now and then. The firmware's README records what was measured and when.

For the appliance: run `provision.sh` on a Pi (re-running it is the upgrade
path, so it must be safe to repeat), then check with the commands each section
of `appliance/README.md` gives.

The proxy and the scripts can at least be syntax-checked off the Pi:

```bash
python -m py_compile proxy/*.py appliance/renew_login.py appliance/setup-ap/setup_ap_server.py desktop/*.py
python3 desktop/gen_faults.py --check      # fault tables in step with faults.cpp
node --check proxy/display/display.js     # if node is at hand
for s in appliance/*.sh appliance/*/*.sh; do bash -n "$s"; done
```

## The desktop monitor and the display page

Both read `nohistory` by the firmware's rules, so test them on the same
cases: a saved answer with its fields changed (the reading's
`lastConduitUpdateServerDateTime`, `lastSG.sg`, `lastAlarm`), shown with
`python3 desktop/minimed_desktop.py --check --file case.json`. The page can
be checked the same way through a stub around the real handler that serves
the edited answer, opened in a browser or with `google-chrome --headless
--dump-dom`. Done on 2026-11-16 for: a current reading, an empty answer
(with and without the proxy asking for a login), a reading 65 minutes old,
no sensor value, a low alarm two minutes old, the same with the glucose
recovered (fault 802), an alarm 16 minutes old, an unparseable alarm time,
an unknown fault id, a pump banner, the pump out of range, and every field
null. Desktop and page gave the same answer each time. HTML in a status
field showed as text on the page, under its CSP, and ran nothing.

On a real Pi, 2026-11-17: a staging set provisioned from the branch
(`provision.sh`, normal switches), files checked by checksum, proxy
restarted with its token kept. From the network: `/display` and its three
files with the security headers, a 404 for every other path under it
(sent unaltered with `curl --path-as-is`, `..` and `%2F` included),
`nohistory` and `fallback` unchanged, the page rendering live data in
Chrome, and `--check` reading it. The page, the desktop monitor from the
command line and from its menu launcher (`install-linux.sh`, Linux Mint
22.3, Cinnamon) were then used by hand and worked.

The name check (`appliance/mdns-name/`), 2026-11-19 on a staging set on
WiFi only: provisioned from the branch, timer enabled, a run with the right
name did nothing. Then the Pi was renamed by hand
(`busctl ... SetHostName s <name>-2`) at 00:16:32; the next timer run, at
00:18:20, logged the wrong name and restarted avahi, and the desktop
resolved the Pi's own `.local` name again. The real trigger, a WiFi drop
that makes avahi rename the Pi, has not been reproduced on purpose.

## The trend screen

2026-11-20, on a staging set: the proxy with `/carelink/trend` installed on
the Pi by hand (checksum against the working tree) and restarted with its
token kept; `nohistory` unchanged (200), `trend` 200 with 191 bytes. The
board on USB, flashed, fetched it on its first cycle (`trend: 36 of 36
readings`; fetch and parse together 159 ms on plan A). The screen was
checked on the panel against live data: line, dot, dashed 70 and 180, dots
below 70, hour labels. Those were `08:00 09:00 10:00` at first, which ran
together; changed to hours only and reflashed, checked on the panel.
`get_trend_data()` was checked on a saved day of CareLink data and on empty
input (no `patientData`, no `sgs`, only zeros: all 404).

The desktop monitor and the display page draw the same chart, 2026-11-20:
the monitor against carelink-piet (`--check` prints the trend line), the
page in headless Chrome under the proxy's CSP (no violations) with live data,
and with a made-up trend for what live data did not show: gaps break the
line, a lone reading is a small dot, and a trend ending 40 minutes ago
stops that far short of now. Not yet seen in a phone browser, or on Windows
and macOS.

The scale follows the readings (it was a fixed 40-300 until the same day):
the three implementations give the same scale for the same seven cases,
from no readings (60-200) to 40 and 400 (40-400). Seen on the page with
live data (60-200, the line about twice as tall) and with a made-up peak
near 290 (top 300, labelled); on the desktop with made-up values. On the
panel the `200` label of the narrowest scale came too close to 180 and was
dropped: the top is labelled only above 200. The zone above 180 is now
dotted like the one below 70. Checked on the panel with live data. Then a finer dotted line at 100, the
pump's target, on all three (its label left out where it would run into
70's); seen on the page with live data, checked on the home board's panel.

Not yet seen: the trend across a DST change (expected: an hour's gap or
overlap for three hours), a sensor gap on the panel (the line should break),
a high peak on the panel (top labelled), and a display against a proxy
without the URL ("No trend from proxy", the other screens unaffected).

## The pump's banner before "No sensor data"

2026-11-20, with the pump really suspended (`pumpBannerState`
`DELIVERY_SUSPEND`, `lastSG` empty with `NO_DATA_FROM_PUMP`, pump and phone
in range): the desktop monitor's `--check` against the reference proxy shows
`DELIVERY SUSPEND [pump]` instead of "No sensor data" (now worded "Pump
suspended" on all three); the travel board, flashed, logs the banner. Seen on the panel too (user, same evening): "Pump suspended" shown
over "No sensor data", bold titles and trend labels fine. Not yet seen:
the page with live data.

## The WiFi watchdog's radio cycle

2026-11-20 on a staging set on WiFi only: the router's 2.4 GHz channel
change (every 6 hours) left the chip failing every WPA handshake for 45
minutes, and the watchdog's retries never cleared it; switching the radio
off and on did. With the new watchdog installed (Pi on Ethernet, run with
`IGNORE_ETHERNET=1` and `wlan0` disconnected in software): with one failure
recorded it cycled the radio and was back on the network in 10 s, counter
cleared. With `nmcli con up` forced to fail by a stand-in: run 1 no cycle
(counter 1), runs 2 and 3 cycle the radio (counter 2, 3), the next real run
reconnects and clears the counter; a connected run clears a leftover
counter. Not yet seen: the next real channel change wedging the chip and the
watchdog clearing it unattended.

## Not tested yet

- **The alarm sound heard by ear** (desktop *Monitor → Test sound*, and the
  page after *Enable alarm sound*), and the monitor **on Windows and
  macOS**: install from python.org,
  the `.pyw` double-click, the sound through `winsound` and `afplay`, and the
  settings file. Also a whole night with the computer awake, to see that the
  window keeps updating.
- **The display page in a phone browser**, and on the reference Pi (it runs
  `STAY_HOME=1`, which leaves the page in place: the page is not a radio
  feature).

- **Quicker travel hand-over**: partly tested on 2026-11-15, on a trip, with
  the Pi on a phone's WiFi. The Pi, rebooted: its WiFi up at 17 s, the
  hotspot at 25.5 s (126 s before). The display, with the hotspot taken down
  for two minutes under it: one scan, `failed after 10922 ms (1 in a row) ...
  next look in 60 s`, `slot 2 is out of range, forgetting it`, 13.9 s awake;
  60 s later no attempt on the forgotten network, one scan, joined in 4.8 s
  and fetched, 7.9 s awake. Not yet seen: the later steps (60, 60, 120, then
  300 s) during a longer outage, "No link" still appearing only after 10
  minutes, and the setup hotspot still coming up on a boot with no network
  now that its service has an `ExecStopPost`.
- **The proxy's status line and quick retry after an outage**: dry-run on
  the desktop on 2026-11-15 against a scripted stub client (network down,
  data, connection error, refused refresh, dead token, network down again):
  the page showed each state with its reason, the recovery form appeared only
  while a token was wanted, retries stayed at 60 s while CareLink was out of
  reach and doubled (60, 120, 240 s) only after a refusal, and
  `/carelink/nohistory` stayed `""` without data. Not yet seen on a Pi: a
  real refused refresh from CareLink (the HTTP code it answers is assumed to
  be 400, 401 or 403), and a real network outage.
- **The display's "Change settings" refill**: reopening the setup page on a
  configured board and checking that every field comes back filled in.
- **The travel hotspot's revert**: on a network with no 2.4 GHz radio, the Pi
  should stay on 5 GHz and run no hotspot.
- **The travel hotspot's tie to one 2.4 GHz radio**: tested on carelink-01
  on 2026-11-14. Forced to 5 GHz, the Pi was tied back to the 2.4 GHz radio
  by the script's next run (22 s) and stayed there. Booted with a tie to a
  radio that does not exist, it let go 30 s into the boot and joined at once,
  with no setup hotspot. Not yet run: the hourly retry on a network whose
  2.4 GHz radio does not take the Pi, and a tied radio that disappears while
  the Pi is running.
- **One address per network port** (`/etc/sysctl.d/50-carelink-arp.conf`):
  on carelink-01 since 2026-11-13 and still set after a reboot. Not yet
  checked: with cable and WiFi both connected, the cable port no longer
  answers for the WiFi address, and both hotspots still hand out addresses.
- **`FORCE=1` on the setup hotspot while the travel hotspot is up**: the
  travel hotspot should stand aside and come back afterwards.
- **The automatic setup hotspot, rehearsed at home**: a Pi booted with no
  known network raising the hotspot by itself. Only the forced path has been
  exercised.
- **`STAY_HOME=1 ./provision.sh`**: first run on a Pi on 2026-11-15, on the
  reference Pi that is also the house DNS server, with `REBOOT_TIME=02:00`.
  It installed no radio features, kept the 02:00 reboot, and left DNS
  working; afterwards every installed file matched the repository, or
  differed from a travelling Pi's only by user name and reboot time. Not yet
  tried: a first install with `STAY_HOME=1` on a fresh Pi.
- **The browser flasher page** (`docs/flasher/`): first release `v0.1-arduino`
  published on 2026-11-15 and used the same day to update a display in use,
  *without* erasing: flashed without a problem, settings kept. Not yet tried:
  *with* erasing, on a new or wiped board, which should end in the setup
  portal.
