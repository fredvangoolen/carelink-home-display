# Taking the set somewhere else: plan A and plan B

At home the display joins the household WiFi and asks the proxy on the Pi.
Away from home that can fail in two places: the Pi does not know the WiFi
there, and the display does not know it either (or joins it and still cannot
reach the Pi). Three features cover this. They are split over the firmware and
the appliance, but they are one design and only make sense together.

| | What | Where it lives |
|---|---|---|
| **Setup hotspot** | gets the *Pi* onto a new WiFi with a phone | `appliance/setup-ap/` |
| **Plan A** | the display reaches the proxy over the household WiFi | firmware, WiFi slots 0 and 1 |
| **Plan B** | the display reaches the proxy over the Pi's own hotspot, `carelink-travel` | firmware slot 2 + `appliance/travel-ap/` + proxy `GET /carelink/fallback` |

## What happens on a trip

1. **Plug the Pi in.** With no network at all for 90 seconds it raises
   `<hostname>-setup` for 5 minutes. Scan the label on the Pi with a phone,
   pick the WiFi, type its password. The Pi joins and remembers it.
2. **The Pi starts `carelink-travel`**, a private WPA2 hotspot at 10.43.0.1,
   next to its own WiFi connection and on the same channel. It is always up
   once the Pi has a network — at home too.
3. **The display wakes.** It scans, finds no plan A network it knows (or
   joins one and gets no answer from the proxy), and switches to
   `carelink-travel` within the same wake. It stays there, trying plan A
   again once an hour, and goes back as soon as the proxy answers there.
   A display that wakes before the hotspot is up finds nothing, and looks
   again after 1, 2, 3 and 5 minutes, then every 5 minutes.

Nobody configures the display for the trip. Optionally, the holiday WiFi can
be put in the display's slot 1 through its setup page; then it uses plan A
there too, if that network lets it reach the Pi.

## How the display knows the hotspot's password

It learns it at home. The proxy serves the hotspot's name and password at
`GET /carelink/fallback` (`{"ssid": ..., "psk": ...}`, or 404 when the Pi has
no hotspot). After a cold boot and then once a day, the display asks — only
over plan A, and only in a cycle where the proxy has already answered — and
writes its slot 2 when the answer differs. So:

- nobody types a 24-character random password into a phone;
- a re-provisioned Pi's new password reaches the display at home, instead of
  failing on a trip;
- a Pi without the hotspot (`TRAVEL_AP=0` or `STAY_HOME=1`) answers 404, and
  the display leaves slot 2 as it is.

The JSON shape of that reply is a contract between `proxy/` and `firmware/`.
Change both or neither.

## Limits

- **One radio, one channel.** A Pi 4 can be one WiFi client and one access
  point at once, on one channel, and the display is 2.4 GHz only. So the Pi
  moves its own WiFi to 2.4 GHz to run the hotspot, and ties itself to that
  one radio of the router so that the router cannot steer it back to 5 GHz.
  On a network with no 2.4 GHz radio there is no hotspot.
- **One access point at a time.** The travel hotspot stands aside while the
  setup hotspot is up.
- **The hotspot reaches the proxy and nothing else**: DHCP, mDNS and port
  8081. No SSH, no internet.
- **A Pi that never leaves the house** needs none of this:
  `STAY_HOME=1 ./provision.sh` installs the proxy without any of the radio
  features.

## Details

- Display side — the slots, the timings, when it switches and when it goes
  back: [`firmware/minimed-coreink/README.md`](../firmware/minimed-coreink/README.md),
  "Several WiFi networks" and "Plan B: the Pi's hotspot".
- Pi side — the hotspots, their firewalls, the passwords and the label:
  [`appliance/README.md`](../appliance/README.md), "Fallback setup hotspot",
  "WiFi watchdog" and "Display fallback hotspot".
- Changing one WiFi slot on a display without redoing its setup, a
  development and support tool (setting the display up again is the normal
  route): [`firmware/tools/nvs-slot-edit/`](../firmware/tools/nvs-slot-edit/).
- What was measured, and what has not been tested yet:
  [testing.md](testing.md).
