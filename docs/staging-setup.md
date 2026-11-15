# What you need on your own computer to stage a set

Staging means preparing a Pi and a display on your own desk before they go to
a household ([runbook.md](runbook.md)). You do not need to be a developer, and
you do not need a development environment. You do need to be comfortable
typing commands into a terminal exactly as written, and careful with
passwords.

There are two routes for the display. Take the first unless you want to change
the firmware.

## For the Pi

Everything is installed *on the Pi*, by one script. Your computer only has to
write the SD card and open a terminal on the Pi.

| You need | Windows | macOS | Linux |
|---|---|---|---|
| **Raspberry Pi Imager**, to write the card | raspberrypi.com/software | same | same, or your package manager |
| **An SSH key**, so the Pi needs no password | PowerShell: `ssh-keygen -t ed25519` | Terminal: same | same |
| **An SSH client** | built in (PowerShell) | built in (Terminal) | built in |
| **A Raspberry Pi ID** with two-factor on, for remote access later | connect.raspberrypi.com | same | same |

`ssh-keygen` asks where to save the key (press Enter) and for a passphrase
(choose one). It prints the name of a file ending in `.pub`: that is the
public key Raspberry Pi Imager asks for. The file without `.pub` never leaves
your computer.

Then follow the runbook from "Staging the Raspberry Pi". Step 3 there is the
whole installation:

```bash
git clone <this repository> && cd carelink-home-display
./appliance/provision.sh
```

**The label** for the Pi's setup hotspot is the one step that runs on your
computer: `appliance/make-label.sh`. It is a bash script and needs either
`qrencode` or [`uv`](https://docs.astral.sh/uv/). On macOS and Linux that is
one install. On Windows, run it from Git Bash (it comes with Git for Windows)
with `uv` installed.

## For the display, without a build environment

Each firmware release is built by GitHub with a fixed set of tool versions and
published in two places:

1. **A web page that flashes it from the browser.** Open the project's flasher
   page in Chrome or Edge on a laptop, connect the Core Ink with a USB-C data
   cable, press the button. The page explains the one question it asks (erase
   or not). The address is
   [https://fredvangoolen.github.io/carelink-home-display/](https://fredvangoolen.github.io/carelink-home-display/).
   For an update of a display in use, choose *do not erase*: its WiFi and
   proxy settings stay. A fork that publishes its own releases gets its own
   page at `https://<owner>.github.io/carelink-home-display/`.
2. **The release itself**, under [*Releases*](https://github.com/fredvangoolen/carelink-home-display/releases/latest) on GitHub: four `.bin` files and
   their checksums, for flashing with `esptool` if the page cannot be used:

   ```bash
   esptool.py --chip esp32 --baud 921600 write_flash \
     0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
   ```

   Add `erase_flash` as a separate command first to wipe the settings too.

Either way, check the result on the display itself: the *Device & Network*
screen shows the version, and it must match the release.

Flashing does not replace the bench test. The runbook's "Watch a full cycle on
the bench" needs a serial console; the flasher page offers one after
installing ("Logs & Console"), and any serial terminal at 115200 baud works.

## For the display, building it yourself

Only needed to change the firmware or to make a test build
([testing.md](testing.md)). Install
[`arduino-cli`](https://arduino.github.io/arduino-cli/) and then exactly these
versions, the ones every measurement in this repository was made with:

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install --no-deps M5GFX@0.2.28 M5Unified@0.2.21 ArduinoJson@7.4.3

FQBN=esp32:esp32:m5stack_coreink:PartitionScheme=huge_app
arduino-cli board list                                    # find the port
arduino-cli compile --fqbn "$FQBN" --upload -p <port> firmware/minimed-coreink
arduino-cli monitor -p <port> --config baudrate=115200    # watch a cycle
```

`.github/workflows/firmware.yml` does the same on every change, so its log is
the reference when a local build misbehaves.

## Making a release

For whoever maintains the firmware:

1. Set `VERSION_STR` in `firmware/minimed-coreink/screens.h`, flash that build
   and verify it on a board ([testing.md](testing.md)).
2. Commit, then tag the commit `v<VERSION_STR>` (for `0.2`, the tag is `v0.2`)
   and push the tag.
3. The workflow refuses a tag that does not match `VERSION_STR`. Otherwise it
   builds, creates the release, and updates the flasher page.

The flasher page needs the repository to be public, with *Settings → Pages →
Source* set to *GitHub Actions*. Until then the release is still made and the
page step is skipped.
