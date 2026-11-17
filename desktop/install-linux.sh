#!/bin/sh
# Adds the CareLink desktop monitor to the applications menu on Linux, for
# this user only. Run it from wherever the desktop/ folder will stay; move
# the folder later and run it again.
#
#   ./install-linux.sh              add to the applications menu
#   ./install-linux.sh --autostart  also start it at login
#   ./install-linux.sh --remove     take both away again
#
# Copyright 2026, Frederik. GPL-3.0; see LICENSE.

set -e

dir=$(cd "$(dirname "$0")" && pwd)
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
autostart="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
name=carelink-monitor.desktop

if [ "$1" = "--remove" ]; then
  rm -f "$apps/$name" "$autostart/$name"
  echo "Removed from the applications menu and from autostart."
  exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found. Install it first (Debian/Ubuntu/Mint: sudo apt install python3 python3-tk)." >&2
  exit 1
fi
if ! python3 -c 'import tkinter' 2>/dev/null; then
  echo "Python has no Tk. Install it first (Debian/Ubuntu/Mint: sudo apt install python3-tk)." >&2
  exit 1
fi
# A launcher's Exec line gives these characters a meaning of their own.
case "$dir" in
  *[\"\`\$\\]*) echo "Move the folder to a path without \" \` \$ or \\ in it, then run this again." >&2; exit 1 ;;
esac

write() {
  mkdir -p "$(dirname "$1")"
  cat > "$1" <<EOF
[Desktop Entry]
Type=Application
Name=CareLink monitor
Comment=Glucose and pump status from the CareLink proxy
Exec=python3 "$dir/minimed_desktop.py"
Path=$dir
Icon=utilities-system-monitor
Terminal=false
Categories=Utility;
StartupWMClass=Carelink-monitor
EOF
}

write "$apps/$name"
echo "Added to the applications menu: CareLink monitor."

if [ "$1" = "--autostart" ]; then
  write "$autostart/$name"
  echo "It also starts at login now. Set the computer not to sleep while plugged in."
fi
