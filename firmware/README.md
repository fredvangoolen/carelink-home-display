# Display firmware

Firmware for the M5Stack Core Ink that shows the pump data.

## Install it from the browser

**[Flash the CareLink home display](https://fredvangoolen.github.io/carelink-home-display/)**:
Chrome or Edge, a USB-C data cable, one button, about a minute. It installs
the [latest release](https://github.com/fredvangoolen/carelink-home-display/releases/latest).
For a display in use, choose *do not erase*: its WiFi and proxy settings
stay. Then the display's own setup page asks for the home WiFi and the
proxy's address ([docs/staging-setup.md](../docs/staging-setup.md)).

## What is here

| Directory | What |
|---|---|
| [`minimed-coreink/`](minimed-coreink/) | the firmware itself: build instructions, measured timings, hardware quirks |
| [`tools/nvs-slot-edit/`](tools/nvs-slot-edit/) | a one-shot sketch to change one WiFi slot without redoing the setup; for development and support, the setup page is the normal route |

Building it yourself with `arduino-cli` is only needed to change the
firmware: [`minimed-coreink/README.md`](minimed-coreink/README.md).

Licence: GPL-3.0, see [LICENSE](LICENSE).
