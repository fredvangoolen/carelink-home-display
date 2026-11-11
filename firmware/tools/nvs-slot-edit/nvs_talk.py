"""Drive the nvs-slot-edit sketch over serial.

usage:
  nvs_talk.py PORT dump
  nvs_talk.py PORT set SLOT SSID        password on stdin, never echoed or in argv
  nvs_talk.py PORT backup SLOT
  nvs_talk.py PORT restore SLOT
  nvs_talk.py PORT rename SLOT SSID
  nvs_talk.py PORT clear SLOT           slot 1 or 2

The password comes from stdin so it can be piped straight from where it
lives, e.g. the Pi's hotspot password into the fallback slot:
  ssh admin@carelink-<name>.local sudo cat /etc/carelink/travel-ap.psk \
    | python nvs_talk.py COM4 set 2 carelink-travel
"""
import sys, time, serial

if len(sys.argv) < 3 or sys.argv[2] not in ("dump", "set", "backup", "restore", "rename", "clear"):
    sys.exit(__doc__)
port, cmd = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=0.5)
s.dtr = False; s.rts = False          # do not hold the chip in reset/bootloader
time.sleep(2.0)
s.reset_input_buffer()
if cmd == "dump":
    s.write(b"DUMP\n")
elif cmd == "rename":
    s.write(f"RENAME {sys.argv[3]}\t{sys.argv[4]}\n".encode())
elif cmd in ("backup", "restore", "clear"):
    s.write(f"{cmd.upper()} {sys.argv[3]}\n".encode())
else:
    slot, ssid = sys.argv[3], sys.argv[4]
    pw = sys.stdin.readline().strip()
    if not pw:
        sys.exit("no password on stdin, refusing")
    s.write(f"SET {slot}\t{ssid}\t{pw}\n".encode())
deadline = time.time() + 8
while time.time() < deadline:
    line = s.readline().decode(errors="replace").rstrip()
    if line:
        print(line)
    if line == "END" or line.startswith("ERR"):
        break
s.close()
