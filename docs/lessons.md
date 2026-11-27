# Lessons

Things that cost a day to find out. Each one looked like something else.

## A mistyped WiFi name looks like a router that isolates its clients

At a holiday house the display never reached the proxy, and the first
conclusion was "this router keeps its clients apart". It did not. The display
had been set up as `Seaview12` for a network called `seaview 12`; it never
joined at all. That router does **not** isolate clients on 2.4 GHz.

What changed because of it: the display's setup page now lists the networks it
can see, warns when a typed name is close to a listed one ("did you mean
`seaview 12`?"), and tests the connection before saving.

Check the name, character by character, before blaming the network.

## Phones capitalise form fields

A phone keyboard turns `office` into `Office` by itself, and WiFi names and
passwords are case-sensitive. A setup page should switch auto-capitalisation and
auto-correct off on those fields (the display's page does; the Pi's setup
page does not yet). Host names are harmless (`Carelink-01.local`
resolves like `carelink-01.local`; mDNS ignores case).

## A Pi 4 has one radio and one channel

It can be one WiFi client and one access point at the same time, but only on
one channel (`iw list`: `#{ managed } <= 1, #{ AP } <= 1, #channels <= 1`).
The display is 2.4 GHz only. So a Pi connected on 5 GHz cannot serve the
display a hotspot until it moves itself to 2.4 GHz. See
[travel.md](travel.md).

A router that steers its clients between bands undoes that move. Asking
NetworkManager for the 2.4 GHz band is not enough: the Pi joins there and
follows the router's request to 5 GHz seconds later. Tying the connection to
the 2.4 GHz radio's own address holds.

## A Pi on cable and WiFi answers for both addresses on both ports

Linux answers an address request (ARP) for any of its addresses on whichever
port the request arrives. With the cable in, the Pi announced its WiFi address
with the cable port's hardware address, and traffic for the WiFi address came
in over the cable. That works until the cable is pulled. `provision.sh` now
makes each port answer for its own address only.

It was found while chasing a night in which a third of one display's fetches
failed with the Pi on WiFi only: the Pi's answer arrived half, then nothing
for 30 seconds. That fault stopped when the Pi's WiFi connection was renewed
and could not be brought back in two days of trying, so its cause is not
known; the address mix-up was real but was not shown to be it.

## A Core Ink that is off looks exactly like one that has locked up

E-paper keeps its last image with no power, so a board that has switched off
shows a plausible, stale reading. On battery the button on the back is a bare
reset line and switches the board *off*: a chip held in reset cannot drive the
pin that latches the power rail on. Hold the PWR button, or plug in USB, to
bring it back. The firmware's README has the measurements ("Power hold, from
reset").

## A blank panel is a wake that never finished

Every wake starts by wiping the panel white and draws only at the end, so a
blank panel (not a stale one) means the board woke and never got to its
draw: a hang or a reset part-way, and on battery a board that may then stay
awake or switch off. Seen once, 2026-11-27, on the home board at 67%, at a
spot with -88 dBm WiFi; reset and start brought it back, and nothing was
left to say which it was. Since then a watchdog bounds every poll and the
next boot redraws the last reading (docs/display-messages.md, "When a poll
does not finish"); the debug screen's *Watchdog* and *Brownout/crash* rows say
which it was next time, and a brownout now leaves *Please charge* on the
panel instead of a blank one.

## A weak USB port passes every test except WiFi

A marginal supply cannot source the radio's power-up inrush. Flashing and the
serial console draw almost nothing, so everything works until the radio
starts, and then the board resets — in a loop, on the splash screen, looking
like a software hang. Watch the serial console across a reset: repeated
`POWERON_RESET` is the supply, not the code.

## NetworkManager gives up on a network after one bad handshake

On a headless Pi, a disconnect during the WPA key exchange is treated as a
wrong password. It asks for a new one, finds nobody to ask, and stops
connecting to that network for good. One router hiccup was enough to keep a Pi
offline for 10 hours. That is what `appliance/wifi-watchdog/` is for.

## A router's channel change can wedge the Pi's WiFi chip

The Pi's radio runs its own WiFi and the travel hotspot on one channel. A
household router that changed its 2.4 GHz channel every 6 hours dropped the
Pi each time; usually it rejoined within minutes, but once the chip came out
of it failing every WPA handshake, with the right password, for 45 minutes
while the watchdog retried. Switching the radio off and on cleared it at
once, so the watchdog now does that from its second failed run. If it
happens often at one address, fixing the router's 2.4 GHz channel removes
the trigger.

## avahi can lose the Pi's name and not take it back

After a WiFi reconnect, avahi may report a host name conflict, probably on
its own echoed announcement, and rename the Pi `carelink-01-2.local`. It
never renames it back. The Pi is up and the proxy is fine, but the display
looks up the old name and finds nothing. Unplugging the Pi's cable makes
this more likely to hurt: the Pi's address then changes, so the address the
display had cached stops working too, and only the name is left.
`appliance/mdns-name/` restarts avahi when the name is wrong.
