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
python -m py_compile proxy/*.py appliance/renew_login.py appliance/setup-ap/setup_ap_server.py
for s in appliance/*.sh appliance/*/*.sh; do bash -n "$s"; done
```

## Not tested yet

- **The display's "Change settings" refill**: reopening the setup page on a
  configured board and checking that every field comes back filled in.
- **The travel hotspot's revert**: on a network with no 2.4 GHz radio, the Pi
  should stay on 5 GHz and run no hotspot.
- **The travel hotspot's tie to one 2.4 GHz radio, as the script makes it**:
  the tie itself was made by hand on carelink-01 and held for 22 hours
  against a steering router (2026-11-12). Not yet run: the script choosing
  the radio and making the tie, its hourly retry, and letting go of the tie
  when the Pi's WiFi has no network (a boot with the tied radio gone
  included).
- **One address per network port** (`/etc/sysctl.d/50-carelink-arp.conf`):
  what it prevents was captured on carelink-01; the setting has not been on
  a Pi yet. To check: with cable and WiFi both connected, the cable port no
  longer answers for the WiFi address, and both hotspots still hand out
  addresses.
- **`FORCE=1` on the setup hotspot while the travel hotspot is up**: the
  travel hotspot should stand aside and come back afterwards.
- **The automatic setup hotspot, rehearsed at home**: a Pi booted with no
  known network raising the hotspot by itself. Only the forced path has been
  exercised.
- **`STAY_HOME=1 ./provision.sh`**: added when the two repositories were
  merged; syntax-checked only, not yet run on a Pi.
- **The browser flasher page** (`docs/flasher/`): it can only be served once
  the repository is public and has a release, so it has never flashed a
  board. The first release should be tried on a spare board, both with and
  without erasing.
