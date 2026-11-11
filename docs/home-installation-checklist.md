# Home Installation Checklist

Written October 2026; last checked against the code on 2026-11-11.

What to do in the patient's home with a set staged on the bench. The Pi is provisioned but holds **no token yet**: the proxy is activated on site, once the Pi is on the home network, by signing in with the patient's care-partner account. The Core Ink is wiped, so it starts in setup mode. Plan 45 minutes. Background and reasons are in the [Staging & Support Runbook](runbook.md).

## Before the visit

Check these by phone or message beforehand. Without any one of them, the visit ends half done.

- [ ] **Home WiFi credentials.** The network name and password. The display needs a 2.4 GHz network, so ask whether the router offers one (most do, often under the same name).
- [ ] **A free Ethernet port on the ISP gateway**, with room for the Pi and a power socket next to it. Without one, the Pi goes on the household WiFi instead, through its setup hotspot (step 1).
- [ ] **The patient's care-partner account is set up.** A CareLink account made for this set alone, invited as a care partner from the patient's own account, the invitation accepted, two-factor on. It must not be signed in anywhere else: one account carries one live session.
- [ ] **Whoever holds that account's password and two-factor device will be there**, or reachable during the visit, to complete the sign-in.

## Bring

- [ ] The Pi, its own power adapter and an Ethernet cable
- [ ] The Core Ink, wiped, and a USB power adapter (not a laptop port)
- [ ] A laptop for the sign-in (a phone works, but the CareLink page is cramped on it), and a phone for the QR code
- [ ] From your records: the Pi's hostname (`carelink-<name>`) and its VNC password

## 1. Connect the Pi

1. Ethernet from the Pi to a free LAN port on the ISP gateway, then power it with its adapter.
2. Wait 2 minutes. On connect.raspberrypi.com the Pi shows as online. Open its remote shell; everything below that says *in the shell* happens there.
3. In the shell, note the Pi's address: `hostname -I`.

**No Ethernet port?** Leave the cable out and power the Pi. After about 1.5 minutes it raises **`carelink-<name>-setup`** for 5 minutes. Scan the QR label on the Pi with your phone; the setup page opens by itself (otherwise browse to `http://10.42.0.1`). Pick the household WiFi, enter its password, Connect. The hotspot disappears and the Pi keeps that WiFi from then on. Missed the 5 minutes: power-cycle the Pi and it comes back. Then continue at step 1.2.

## 2. Activate the proxy

1. In the shell: `/opt/carelink-renewal/renew.sh`. It prints a link and waits 15 minutes.
2. On the laptop, joined to the household WiFi, open `http://carelink-<name>.local:6080/vnc.html`. If the name does not open but `http://<address>:6080/vnc.html` does, the network does not pass mDNS: see *If something goes wrong* below, and use the address from here on.
3. Enter the VNC password. In the browser that appears, the account holder signs in to CareLink with the **care-partner account** and passes its checks: a tick box or picture CAPTCHA, and a two-factor code. Expect a red "without HTTPS" warning and a grey screen first; both are normal. When the sign-in succeeds, the browser **vanishes**. That is the success signal.
4. The shell reports *renewal complete*. Start the proxy for good: `sudo systemctl enable --now carelink2-proxy`.
5. Check it: `curl -s http://localhost:8081/carelink/nohistory | head -c 300` shows the **patient's first name**. Another name means the account is linked to the wrong patient: stop here.
6. The proof comes later: the first token refresh, about 3 hours on. Check it remotely in the evening: `sudo journalctl -u carelink2-proxy | grep -A1 _do_refresh`.

## 3. Set up the display

1. Power the Core Ink. The panel shows a QR code with the setup network's name and password.
2. Scan the QR code with the phone and join. The setup page opens by itself; if not, browse to `http://192.168.4.1`.
3. Fill in, and let the household type their WiFi password:

| Field | Enter |
| --- | --- |
| WiFi name and password | pick the household network from the list, do not type its name; the list shows only 2.4 GHz networks, which is what the display needs |
| 2nd network | leave empty |
| Fallback network | leave empty; the display learns the Pi's own hotspot by itself |
| Proxy | `carelink-<name>.local` (or the reserved address, if mDNS does not work) |
| Port | `8081` |
| Time zone | `1` (pre-filled; summer time is automatic) |
| Patient | leave empty; the proxy supplies the name |

4. Press **Test and save**. The display joins the WiFi and looks for the proxy before it stores anything; this takes up to a minute, and the phone may drop off the setup network for a moment. The result page (and the panel) says what worked.
5. All good: save. A warning: read it. A wrong password, a name that is nearly right ("Did you mean …?") and a proxy that does not answer each get their own message. *Change settings* goes back to the form as it was.
6. The display restarts and shows a reading within a minute.

## 4. Place the display

1. Pick a spot people pass, such as the kitchen or the hallway, not a bedside.
2. Flick the switch to *Device & Network* and wait for the next refresh, at most 5 minutes. **Signal** must read *fair* or *good*. If it reads *weak*, move the display closer to the gateway and check again.

## 5. Prove it

- [ ] The display shows the patient's name, a current reading, and a time within a few minutes of the clock.
- [ ] **The Communication screen** (last page, after *Device & Network*) reads: *Pump near phone: yes*, *Phone online: yes*, *Proxy answer: reading*, *Good fetch: now* or a few minutes ago, and no failures.
- [ ] **Power cut, in front of them:** unplug both, wait 10 seconds, plug both back in. A current reading must return within 10 minutes.
- [ ] In Connect's remote shell: `systemctl is-active carelink2-proxy` reads `active`, and `ss -tlnp | grep -E ':6080|:5900'` prints nothing.

## 6. Hand over

- [ ] The four things from the runbook's *Handing it over*: what the display shows, what normal looks like (including the message line at the bottom), how to tell it is stuck (the time on the screen), and the renewal call.
- [ ] For a renewal call they need: the link `http://carelink-<name>.local:6080/vnc.html`, the VNC password, and the care-partner account's sign-in with its two-factor code.
- [ ] Leave the VNC password written down where they will find it in six months.
- [ ] Show them which plug is which.
- [ ] Note the gateway's make and model.

## If something goes wrong on site

Look first at the display's **Communication screen**, the last page: it follows pump → phone → CareLink → proxy → display and usually names the broken hop. *Pump near phone: no* or *Phone online: no* is the patient's phone, not the set. *Proxy answer: no answer* is the Pi or the WiFi. *Proxy answer: login needed* is the token. *Good fetch: now* while *Last upload* keeps growing means the proxy is stuck: restart it. The [runbook](runbook.md#the-displays-communication-screen) explains every row.

| You see | Do |
| --- | --- |
| The renewal link opens by address but not by name | The network does not pass mDNS. Give the Pi a DHCP reservation on the gateway, and enter that address on the display instead of the name |
| *No link to proxy* on the display after 15 minutes | Check the WiFi is 2.4 GHz, and that the WiFi name and the proxy name are spelled exactly right, capitals and spaces included. To redo the setup: on *Device & Network*, hold the switch in for 5 s, choose *Yes* |
| The display stays on the version splash | The power adapter is too weak. Replace it |
| *CareLink login needed* | Right after a power cut or a cable change: in the shell, `sudo systemctl restart carelink2-proxy` and wait 5 minutes. Only if that does not help is the token gone: run a renewal with the care-partner account (runbook, Staging the Pi, step 5) |
| Signal reads *weak* | Move the display, or add a WiFi access point near it |
