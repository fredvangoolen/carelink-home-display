# What the display says, and why

Every message the main screen can show in its bottom strip, which one wins
when several apply, what triggers each and when it clears. It describes the
Core Ink firmware; `desktop/minimed_desktop.py` and the proxy's `/display`
page apply the same rules (CLAUDE.md, "The contract between the two
halves"), with the differences listed at the end.

For households, [manual.md](manual.md) lists the messages and what to do.
For support at a distance, [runbook.md](runbook.md), "Reading a fault from a
distance", maps each message to the link in the chain that broke.

## One strip, one message

The strip at the bottom of the *Glucose* screen shows one message. The
firmware takes the first rule that matches, in this order
(`draw_main_screen()` in `screens.cpp`):

| # | Shows | When | Clears |
|---|---|---|---|
| 1 | **Pump alarm**, e.g. *Low Sensor Glucose (14:05)* | `lastAlarm` less than 15 minutes old, with a `faultId`. Text from the fault table; the time in brackets is when it happened | 15 minutes after the alarm; a low alarm once the reading is above 70, a high alarm once it is below 180; or a confirmed link fault |
| 2 | **The pump's own banner**, e.g. *Pump suspended* | No reading confirmed, the pump in range of the phone, and CareLink sends a pump banner: that banner is the reason, so it wins over *No sensor data* | When the banner goes or a reading returns |
| 3 | ***No sensor data since 13:40*** | No reading confirmed (3 polls), a reading seen since the board started | On the first poll with a reading |
| 4 | **Link fault**: *No link to proxy*, *CareLink login needed*, *Proxy has no data*, or plain *No sensor data* | The fault confirmed (3 polls) | On the first poll that gets data again |
| 5 | ***No new reading for 25 min*** | Everything answers, but the reading is 20 minutes old or more | When a newer reading arrives |
| 6 | **Pump banner or system status**, e.g. *BG REQUIRED* | Nothing above applies and CareLink sends one | When CareLink stops sending it |
| — | nothing | Default | |

Rule 5 outranks rule 6 on purpose: when the reading is stale, the pump
banner is exactly as old.

## The texts the display writes itself

Everything except the alarm texts (from `faults.cpp`) and the pump banners
(from CareLink) is the display's own wording:

| Text | Means | Shown when |
|---|---|---|
| *No sensor data since HH:MM* | The proxy works, CareLink has no current reading: sensor warm-up or lost signal, pump out of range of the phone, phone offline. Routine, clears itself. The time is that of the last reading | 3 polls in a row with an answer but no glucose value |
| *No sensor data for N days* | The same, past a day, where a clock time would be ambiguous | As above |
| *No sensor data* | The same, with no reading seen since the board started (or no valid clock), so no time to name | As above |
| *CareLink login needed* | The proxy answered with no data and its status page says *Valid token required*. Someone has to sign in again | 3 polls in a row |
| *Proxy has no data* | The proxy answered with an empty body and gave no reason: usually its own internet or a CareLink outage | 3 polls in a row |
| *No link to proxy* | No HTTP answer from the proxy, or no WiFi network joined at all (after plan A and plan B) | 3 counted failures. After a failed WiFi join the quick looks at 60/60/60/120 s do not count |
| *No new reading for …* | Everything answers but the reading does not move: the phone app, or a proxy stuck on one fetch | Reading 20 minutes old or more |
| *Pump suspended* | Plain words for the pump banner `DELIVERY_SUSPEND`, the only pump banner reworded. Others show as CareLink names them, underscores turned into spaces | Wherever a pump banner shows |
| *Unknown error code nnn* | A `faultId` missing from the fault table. Still an alarm, still beeps | Rule 1 |

### When CareLink says "no data" but has readings

CareLink sometimes sends `lastSG` as an entry with `sg 0` and
`NO_DATA_FROM_PUMP` while real readings keep arriving in its history. The
proxy then serves the newest of those readings, if it is at most 10 minutes
old, so the displays show it and not *No sensor data*
([carelink-api.md](carelink-api.md), at the end). A real outage still shows:
once no new reading has come for 10 minutes, the entry passes through.

### Why "since", not an age

The time comes from the last upload that carried a glucose value, kept from
poll to poll (`lastGoodAt`, kept through deep sleep like the bracketed last
value). It is deliberately not the age of `lastConduitUpdateServerDateTime`:
the phone keeps uploading without a glucose value through a sensor gap, so
that age stays under 5 minutes however long the gap lasts. Until 0.7 the
display showed it, and read *No sensor data (1 min)* fifteen minutes into a
gap.

## Three polls before a fault shows

Polls are 5 minutes apart. A fault is shown only on the third poll in a row
that has one (`LINK_FAULT_STREAK`), so one bad poll never raises a message
that is gone again before anyone reads it. A sensor gap builds up like this:

| Poll | Panel |
|---|---|
| 13:40 | *132 →*, the last reading |
| 13:45 | *---* with *[132]*, no message: streak 1 |
| 13:50 | the same: streak 2 |
| 13:55 | *No sensor data since 13:40*: confirmed |
| later | the time stays, so a long gap reads as long; past a day, *for N days* |

For those first 10 minutes only the *Updated … ago* row says anything is
wrong; when the proxy is the problem, the old number stays up with no
message at all. That is the price of the rule, and it is deliberate.

## Alarms

- An alarm shows for 15 minutes from `lastAlarm.dateTime`. That field is
  **local wall-clock time**, not UTC; it is shifted back by the time zone
  for the age check and shown as it is.
- A low alarm drops once the reading is back above 70, a high alarm once it
  is below 180. With no reading (a gap) it stays for its full 15 minutes:
  the cautious side.
- When polls fail, the panel redraws from the last snapshot. Before every
  such redraw the alarm is checked again (`expire_alarm()`): dropped once a
  link fault is confirmed, or once past its 15 minutes. Without that, an
  alarm raised just before the token expired hid *CareLink login needed*
  until a fetch worked again (fixed in 0.7). Without a valid clock the alarm
  stays.
- The buzzer gives one 400 ms beep at 2 kHz per poll while an alarm shows,
  before the panel redraws: about three beeps in a 15-minute window. Link
  faults and stale readings never beep.

## Outside the strip

| What | Rule |
|---|---|
| *---* in place of the number | No current reading. From the first poll without one, without waiting for the streak |
| *[132]* right of *mg/dL* | The last real reading, in brackets so it never passes for a current one. Only while the number is *---* |
| The glucose band inverted | Reading above 180 or below 70. There is no red ink, so the whole band turns black |
| Big or small number | Any message in the strip shrinks the number to make room for two lines |
| Battery icon | Hidden whenever the strip has a message: no room |
| *Updated 4 min ago \| 09:55* | The age of the reading, then the time of this redraw. A clock that stops moving means the display itself stopped |
| Trend screen: *No readings in 3 h* (or *12 h*) | A trend was fetched but holds no reading in the window shown |
| Trend screen: *No trend from proxy* | No trend fetched since the board started |
| Trend screen: *Proxy sends 3 h only* | The 12-hour view, with a proxy too old for `/carelink/trend12h`; the 3-hour view still works |

The strip holds two lines of DejaVu 18 when the text fits, else DejaVu 12;
anything longer is cut at the end of line two.

## Where the desktop and the page differ

- They poll every minute, not every 5. *No link to proxy* waits for 3
  missed polls (3 minutes); the other messages show from the first poll
  that has them.
- They learn the time of the last reading only from a reading they fetched
  themselves. Started during a gap, they show plain *No sensor data* until
  the first reading.
- They play a different sound for an alarm (`lastAlarm.type == "ALARM"`)
  than for an alert; the display has one beep for both.
