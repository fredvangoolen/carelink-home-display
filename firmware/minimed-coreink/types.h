// Shared types for the Core Ink Arduino port.
//
// These live in a header rather than the .ino on purpose: arduino-cli
// auto-generates function prototypes and inserts them ABOVE everything
// defined in the sketch body, so a struct declared in the .ino is not yet
// visible at the point where a prototype mentioning it is injected. Types
// pulled in by #include are.

#pragma once

#include <Arduino.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

// Up to three WiFi networks: home, a phone hotspot for demos on location,
// and one spare. Slot 0 keeps the original NVS key names (wifissid/wifipass)
// so a device configured before multi-SSID existed needs no reconfiguration.
#define WIFI_SLOTS 3

struct WifiNet {
  String ssid, pass;
  bool usable() const { return ssid.length() && pass.length(); }
};

// Hours from UTC, standard time; summer time is added from the proxy's
// clientTimeZoneName. 1 (Brussels), not 0: the portal form shows this value,
// and an untouched 0 put every clock an hour out - the same default the
// MicroPython builds have always had.
static const int DEFAULT_TIMEZONE = 1;

struct Config {
  WifiNet wifi[WIFI_SLOTS];
  String proxyaddr, ntpserver, patient;
  uint16_t proxyport = 8081;
  int timezone = DEFAULT_TIMEZONE;
};

// Why there is no current reading.
//
// The panel must not say the same thing for a dead proxy and a pump that is
// simply out of range: the first needs somebody to act and will not clear on
// its own, the second is routine and clears itself. Before this they were
// both a bare "---".
enum {
  LINK_OK = 0,          // a current reading was fetched
  LINK_NO_SENSOR,       // proxy has data, but no current glucose value
  LINK_PROXY_NO_DATA,   // proxy answered and is holding nothing at all
  LINK_PROXY_LOGIN,     // proxy says outright that it needs a CareLink login
  LINK_NO_PROXY,        // could not reach the proxy
  // The two below are the display's own: the desktop and the page have no
  // WiFi join and no deep-sleep cycle to lose, so they never see them.
  LINK_NO_WIFI,         // joined no network at all, plan A nor plan B
  LINK_ABORTED,         // the cycle never finished: the watchdog cut it off,
                        // a crash, or a brownout reset (see cycle_aborted())
};

// The trend screen: the proxy's /carelink/trend, 36 readings 5 minutes apart,
// and /carelink/trend12h, 144 of them for the 12-hour view. Must match
// TREND_SLOTS, TREND_SLOTS_12H and TREND_STEP_S in
// proxy/carelink_client2_proxy.py.
static const int TREND_SLOTS     = 36;
static const int TREND_SLOTS_12H = 144;
static const int TREND_STEP_S    = 300;

// Mirrors new_state() in main_m5coreink.py. Plain values only, so the whole
// struct can later sit in RTC memory across a deep sleep without the JSON
// round-trip the MicroPython build needs.
// Deliberately a plain POD with NO default member initializers.
//
// This struct is stored in RTC memory across deep sleep. A member with an
// NSDMI makes the enclosing struct non-trivially-constructible, and the
// compiler then emits a dynamic initializer that runs at startup on EVERY
// boot - including a deep-sleep wake. Observed exactly that: the RtcState
// fields around it (magic, cycle, next_poll) persisted correctly while the
// snapshot was silently re-initialised on every wake, so the secondary
// screens always drew "--". Initialise with state_init() instead.
struct State {
  bool haveData;
  int sg;                    // mg/dL, 0 = no reading
  // The last reading that actually arrived, kept so the panel can still show
  // what the number WAS while there is no current one. Survives sg going to
  // 0, which is the whole point: it is carried forward across cycles rather
  // than recomputed, because the state is replaced wholesale on every fetch.
  int lastGoodSg;
  // When that reading arrived: the phone upload that carried it
  // (lastConduitUpdateServerDateTime), epoch UTC; 0 = not seen since boot.
  // Carried forward the same way. lastUpdate cannot stand in for it: the
  // phone keeps uploading without a glucose value through a sensor gap, so
  // lastUpdate stays minutes old while the gap lasts hours.
  time_t lastGoodAt;
  char trend[16];
  float activeInsulin;
  int batteryPct;
  float reservoirUnits;
  int reservoirPct;
  int sageHours;             // 255 = the pump's "no sensor yet" sentinel
  char sensorState[32];
  char patient[32];
  time_t lastUpdate;         // epoch UTC of the reading itself
  char banner[64];
  // Alarm message, and the alarm's OWN occurrence time as LOCAL wall clock
  // (not "now", and not UTC - see resolve_alarm()). Empty/0 = no alarm.
  char alarm_text[80];
  time_t alarm_local;
  char ip[16];
  char ssid[33];             // the network actually in use, for the info screen
  int rssi;                  // its signal at the last poll, dBm; 0 = not measured
  int dstDelta;
  int link;                  // one of LINK_*, why there is no reading
  // Which battery step is showing. Carried rather than recomputed per draw
  // so the hysteresis has something to be sticky against, and so a toggle
  // redraw cannot disagree with the scheduled one. -1 = not sampled yet.
  int battState;
  // 24h distribution, for the stats screen
  int timeInRange, aboveHyper, belowHypo, averageSG;
  // The communication chain, for the serial log's "comms" line: which hop
  // of pump -> phone -> CareLink -> proxy is broken.
  int8_t pumpInRange;        // pump within reach of the phone; -1 = unknown
  int8_t phoneInRange;       // phone in contact with CareLink; -1 = unknown
  int rawLink;               // this poll's LINK_*, before the 3-cycle smoothing
  // When that poll ran, epoch UTC; 0 = none yet. Unlike the rest of the
  // snapshot, these two are written on EVERY poll, failed ones included, so
  // the Device & Network screen says how the last attempt went.
  time_t pollAt;
  uint8_t failStreak;        // polls in a row without a reading
  char proxyIp[16];          // the remembered proxy address
  // The last 12 hours, oldest first, 0 = no reading in that slot; the 3-hour
  // view is the last TREND_SLOTS of them. trendEnd is the newest slot's time
  // as LOCAL wall clock parsed naively (like alarm_local), 0 = no trend
  // fetched yet. trend12h: the proxy sent 12 hours; false = an older proxy
  // with only /carelink/trend, whose 3 hours fill the last slots. Carried
  // across a cycle whose trend fetch fails, like lastGoodSg.
  uint16_t trendSg[TREND_SLOTS_12H];
  time_t trendEnd;
  bool trend12h;
};

// Counters for the debug screen (a short press on Device & Network), kept
// in RTC memory beside the snapshot: since the last power-on, back button or
// deliberate restart, and deliberately NOT reset by the resets they count.
// RAM only - nothing is written to flash, and keeping them costs nothing
// measurable. Plain POD for the same reason as State.
struct DebugStats {
  uint32_t polls;        // scheduled polls started, aborted ones included
  uint32_t pollsOk;      // ... that brought data from the proxy
  uint16_t watchdog;     // polls cut off by the watchdog
  time_t   watchdogAt;   // when the last of those was found, epoch UTC; 0 = never
  uint16_t brownout;     // brownout resets
  uint16_t crashes;      // other crash resets (panic)
  uint16_t noWifi;       // wakes that joined no network at all
  uint16_t noProxy;      // joined, but the proxy did not answer
  uint16_t planB;        // polls made over the fallback hotspot
  uint32_t awakeN;       // polls whose awake time was measured (finished ones)
  uint32_t awakeSumMs;
  uint32_t awakeMaxMs;
};

inline void state_init(State &s) {
  memset(&s, 0, sizeof(s));
  snprintf(s.trend, sizeof(s.trend), "NONE");
  s.activeInsulin = -1.0f;
  s.batteryPct = -1;
  s.reservoirUnits = -1.0f;
  s.reservoirPct = -1;
  s.sageHours = 255;
  s.timeInRange = s.aboveHyper = s.belowHypo = s.averageSG = -1;
  s.link = LINK_OK;
  s.battState = -1;
  s.pumpInRange = s.phoneInRange = -1;
  s.rawLink = -1;
}
