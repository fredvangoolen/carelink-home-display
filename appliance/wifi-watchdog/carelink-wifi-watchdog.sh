#!/usr/bin/env bash
#
# Put a proxy on WiFi back on its network after NetworkManager gave up.
#
# On a headless Pi, NetworkManager treats a disconnect during the WPA key
# exchange as a wrong password, asks for a new one, finds nobody to ask
# ("no secrets: No agents were available") and stops connecting to that
# network until someone intervenes. A router hiccup is enough. carelink-01
# sat offline for 10 hours on 2026-11-01 after its router steered it to a
# radio with an inconsistent WPA2/WPA3 announcement, with the working radio
# and a correct profile right there.
#
# Retrying is not always enough. On 2026-11-20 carelink-piet's WiFi chip
# came out of a router disconnect (reason 8) and a failed roam to the
# router's other radio in a state where every WPA handshake timed out
# ("4-Way Handshake failed - pre-shared key may be incorrect"), 40 times in
# 45 minutes, with the right password and the router fine for everyone
# else. Switching the radio off and on cleared it at once. So from the
# second failed run in a row, the radio is cycled before the next attempt.
# That also drops the travel hotspot for a few seconds; it is down anyway
# while wlan0 has no network, and carelink-travel-ap.timer restores it.
#
# Run every 2 minutes by carelink-wifi-watchdog.timer. Does nothing while
# the Pi is on Ethernet, already on WiFi, showing its setup hotspot, or has
# no saved WiFi network. Otherwise it brings the saved networks up by hand,
# most recently used first, which works where NetworkManager's own retry
# has been switched off.
set -uo pipefail

IFACE="${IFACE:-wlan0}"
AP_CON="carelink-setup-ap"
IGNORE_ETHERNET="${IGNORE_ETHERNET:-0}"   # 1 = act even on Ethernet (testing)
# Failed runs in a row. In /run, so a reboot - which resets the chip anyway -
# starts the count over.
FAILS_FILE="${FAILS_FILE:-/run/carelink-wifi-watchdog.fails}"

log() { echo "wifi-watchdog: $*"; }

devs="$(nmcli -t -f DEVICE,TYPE,STATE,CONNECTION dev 2>/dev/null)"

if [[ "$IGNORE_ETHERNET" != 1 ]] && awk -F: '$2=="ethernet" && $3=="connected" {f=1} END {exit !f}' <<<"$devs"; then
  exit 0                                   # on the cable: WiFi is not needed
fi
wifi_line="$(awk -F: -v d="$IFACE" '$1==d' <<<"$devs")"
[[ -n "$wifi_line" ]] || exit 0            # no WiFi device
IFS=: read -r _ _ state con <<<"$wifi_line"
[[ "$con" == "$AP_CON" ]] && exit 0         # the setup hotspot is up
if [[ "$state" == "connected" || "$state" == connecting* ]]; then
  rm -f "$FAILS_FILE"
  exit 0
fi

# Saved WiFi networks, most recently used first; the hotspot is not one.
mapfile -t profiles < <(nmcli -t -f TIMESTAMP,TYPE,NAME con show 2>/dev/null |
  awk -F: -v ap="$AP_CON" '$2=="802-11-wireless" && $3!=ap && $3!="carelink-travel" {print $1":"$3}' |
  sort -t: -k1,1nr | cut -d: -f2-)
[[ ${#profiles[@]} -gt 0 ]] || exit 0

fails="$(cat "$FAILS_FILE" 2>/dev/null || echo 0)"
[[ "$fails" =~ ^[0-9]+$ ]] || fails=0

log "$IFACE is $state with no network; trying ${profiles[*]}"
if (( fails >= 1 )); then
  log "failed $fails time(s) in a row; switching the radio off and on first"
  nmcli radio wifi off
  sleep 3
fi
nmcli radio wifi on
nmcli dev wifi rescan ifname "$IFACE" >/dev/null 2>&1 || true
sleep 3
for p in "${profiles[@]}"; do
  if nmcli --wait 45 con up "$p" ifname "$IFACE" >/dev/null 2>&1; then
    log "back on '$p'"
    rm -f "$FAILS_FILE"
    exit 0
  fi
  log "could not bring up '$p'"
done
echo $((fails + 1)) > "$FAILS_FILE"
exit 0
