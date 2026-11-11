# nvs-slot-edit

Change one WiFi slot on a configured Core Ink without the setup portal and
without regenerating the NVS image.

The alternatives both cost more. The portal needs the board wiped first and
every setting retyped. An NVS image made with `nvs_partition_gen.py` and
flashed at `0x9000` rewrites *every* key, so all WiFi passwords have to be in
hand, and one can be lost. This edits the `minimed` namespace in place, so
credentials only move key to key and are never typed, echoed or logged.

## Use

```bash
FQBN=esp32:esp32:m5stack_coreink:PartitionScheme=huge_app
arduino-cli board list                                   # find the port
arduino-cli compile --fqbn "$FQBN" --upload -p COM4 firmware/tools/nvs-slot-edit
python firmware/tools/nvs-slot-edit/nvs_talk.py COM4 dump
# ... set / backup / restore / rename, see below ...
arduino-cli compile --fqbn "$FQBN" --upload -p COM4 firmware/minimed-coreink   # put the monitor back
```

`nvs_talk.py` needs `pyserial`.

| command | does |
|---|---|
| `dump` | prints each slot's SSID and password **length**, and the proxy |
| `set SLOT SSID` | makes the slot exactly that network; password on stdin |
| `backup SLOT` | copies the slot to `ssidbak`/`passbak`, keys the firmware never reads; only if the backup is empty |
| `restore SLOT` | copies the backup into the slot |
| `rename SLOT SSID` | new name, password kept (a mistyped SSID) |
| `clear SLOT` | empties slot 1 or 2; slot 0 is required and never cleared |

The board is not monitoring while this sketch is on it. Reflash the monitor
when done.

## Examples

The fallback slot (slot 2, plan B: the Pi's `carelink-travel` hotspot), with
the password piped from the Pi so it never appears anywhere:

```bash
ssh admin@carelink-<name>.local sudo cat /etc/carelink/travel-ap.psk \
  | python nvs_talk.py COM4 set 2 carelink-travel
```

A mistyped SSID, as found at a holiday house where slot 0 said `Seaview12` for
the network `seaview 12`:

```bash
python nvs_talk.py COM4 rename 0 "seaview 12"
```

Temporarily replacing slot 0 while keeping the old entry:

```bash
python nvs_talk.py COM4 backup 0
... | python nvs_talk.py COM4 set 0 <ssid>
python nvs_talk.py COM4 restore 0        # later
```

## Why it is built this way

- **It holds the power latch** (GPIO12) from a global constructor, as the
  monitor does. Without it, a board on battery switches off before `setup()`.
- **Every command is idempotent, keyed on the end state.** An upload ends
  with a hard reset and attaching a console resets the board again, so
  `setup()` runs more than once. A blind swap or toggle would silently undo
  itself.
- **It refuses rather than half-writes.** A partial write can strand a board
  on a network it can no longer reach.
- **Passwords are never printed**, only their lengths. A password reaches the
  board on stdin, never on the command line, where it would land in shell
  history and process lists.
