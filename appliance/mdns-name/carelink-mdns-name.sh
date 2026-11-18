#!/usr/bin/env bash
#
# Give the Pi its own name back on the local network after avahi lost it.
#
# The display finds the proxy by mDNS name (carelink-01.local), so the name
# matters more than the address. When avahi thinks another machine already
# uses that name, it moves to carelink-01-2.local and never moves back, and
# the display cannot find the proxy any more. That happened on a Pi on
# WiFi only on 2026-11-18: the router dropped its WiFi for two minutes, and
# during the reconnect avahi saw a "conflict" (most likely its own
# announcement coming back) and renamed the Pi. A restart of avahi was all
# it took to get the name back; this script does that restart.
#
# Run every 2 minutes by carelink-mdns-name.timer. Does nothing while avahi
# is not running or still has the right name. If a second machine really
# does use the name, each restart ends in the same rename; the journal then
# shows this script's line every 2 minutes, which is the clue to look for.
set -uo pipefail

log() { echo "mdns-name: $*"; }

want="$(hostname)"
reply="$(busctl call org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetHostName 2>/dev/null)" || exit 0
have="${reply#s \"}"; have="${have%\"}"     # reply looks like: s "carelink-01"
[[ -n "$have" ]] || exit 0
[[ "${have,,}" == "${want,,}" ]] && exit 0  # mDNS names are not case-sensitive

log "avahi calls this Pi '$have.local' instead of '$want.local'; restarting avahi"
systemctl restart avahi-daemon
