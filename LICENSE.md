# Licences and credits

This repository holds two works with different licences. Each directory
carries the full text of its own.

| Directory | Licence | Text |
|---|---|---|
| `proxy/`, `appliance/` | GNU Affero General Public License v3.0 | [`proxy/LICENSE`](proxy/LICENSE), [`appliance/LICENSE`](appliance/LICENSE) |
| `firmware/` | GNU General Public License v3.0 | [`firmware/LICENSE`](firmware/LICENSE) |
| `desktop/` | GNU General Public License v3.0 | [`desktop/LICENSE`](desktop/LICENSE) |
| `docs/`, the files in the root | describe both; treat as AGPL-3.0 | |

The display, the desktop monitor and the proxy are separate programs that
talk over HTTP; none contains code of another. The one shared piece is data:
the pump's fault texts, kept in `firmware/minimed-coreink/faults.cpp` and
generated from there into `desktop/faults.py` and `proxy/display/faults.js`
(GPL-3.0 data inside the AGPL-3.0 proxy, which section 13 of both licences
allows).

## Where it comes from

**`proxy/`** is a modified copy of
[carelink-python-client](https://github.com/ondrej1024/carelink-python-client)
by Ondrej Wisniewski and its contributors (Mortalin, Samuel Dezso
Salling-Dysch, Ecoboaea Denis, Scott Newman, Pal Marci), taken in November
2026 and changed since: among other things the `/carelink/fallback` endpoint
and fixes to token and clock handling. The older `client1` implementation was
left out. That project in turn thanks:

- [Pal Marci](https://github.com/palmarci) for reversing the CareLink Cloud
  API communication of the "CareLink Connect" app;
- [Bence Szász](https://github.com/benceszasz) for the Java implementation of
  the xDrip CareLink Follower.

**`appliance/`** was written for this project, on top of that proxy.

**`firmware/`** is a C++ rewrite of a MicroPython monitor that was itself a
redesign of Ondrej Wisniewski's **M5 Minimed Monitor**. The fault-code tables
(reverse-engineered from real pump alarm data) and the overall
poll / configure / setup-hotspot design come from there.

**`desktop/`** is a rewrite of `minimed-mon-pc.py`, the Tkinter version of
the same M5 Minimed Monitor, using the firmware's rules for what to show.

This repository starts with a fresh history. The earlier history of both
halves, including the upstream commits, is not in it; the credits above stand
in for it.
