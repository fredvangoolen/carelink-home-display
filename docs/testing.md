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

Seen on Linux only: the window in three states (screenshots). The sound
files are generated and valid, and `paplay` is found, but nobody has
listened to them yet: use *Monitor → Test sound*.

## Not tested yet

- **The desktop monitor's sound** on any system, and the monitor **on Windows
  and macOS**: install from python.org,
  the `.pyw` double-click, the sound through `winsound` and `afplay`, and the
  settings file. Also a whole night with the computer awake, to see that the
  window keeps updating.
- **The display page on a real Pi**: installed by `provision.sh` into
  `/usr/local/carelink/display/`, reached at `http://<pi>:8081/display`, and
  the sound after a click, in a phone browser as well.

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
