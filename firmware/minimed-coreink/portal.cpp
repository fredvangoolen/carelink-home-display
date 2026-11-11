// First-time setup: an access point serving a config form.
//
// Ported from do_access_point() in main_m5coreink.py. Reached when the
// config is incomplete, which in practice means a device that has never been
// set up - so somebody is standing in front of it. That assumption is why
// this blocks indefinitely rather than timing out, and why it is silent.
//
// Beyond the MicroPython form it guards against typing mistakes, which were
// the costliest setup error so far: a board configured at a holiday house as
// "Seaview12" for the network "seaview 12" never joined it, and that read as a
// router keeping its clients apart until the spelling was spotted.
//   - the networks in range are scanned when the portal starts and offered
//     in a list above each name field, so a name can be picked, not typed;
//   - Save tests before it stores: each network the board can see is joined
//     with the given password, and the proxy is looked for over it;
//   - a name that was not seen is not tested but flagged, with the network
//     it most likely meant ("did you mean 'seaview 12'?").
// Nothing is stored until the person has seen the results and saves.

#include "portal.h"
#include "screens.h"

#include <vector>
#include <algorithm>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <M5Unified.h>
#include <lwip/netdb.h>

static const char *AP_SSID = "M5INK_MINIMED_MON";
static const char *AP_ADDR = "192.168.4.1";

static WebServer server(80);
static String ap_pass;
// Answers every name lookup with the portal's own address. That is what
// makes this a captive portal: a phone's connectivity check (Android's
// generate_204, Apple's hotspot-detect) lands on the form instead of the
// internet, so the phone opens "sign in to network" by itself. Without it a
// phone sees a network with no internet, keeps using mobile data, and
// 192.168.4.1 typed into its browser goes nowhere - the first test of the
// random-password portal on a phone failed exactly that way.
static DNSServer dns;
static bool saved = false;
static bool use_ap = true;
static bool dry_run = false;

// ------------------------------------------------------------------- form

static const int SLOTS = 3;
// Same field names as the MicroPython build, so the two are interchangeable
// to anyone who has set one up before.
static const char *SSID_FIELD[SLOTS] = {"fwifissid", "fssid1", "fssid2"};
static const char *PASS_FIELD[SLOTS] = {"fwifipass", "fpass1", "fpass2"};
static const char *SSID_LABEL[SLOTS] = {
  "SSID", "2nd SSID (optional)",
  "Fallback SSID (optional: the Pi's own hotspot, used only when the networks above do not reach the proxy. "
  "Leave empty: the display learns it from the proxy by itself)"};
static const char *PASS_LABEL[SLOTS] = {"Password", "2nd password", "Fallback password"};

// What was typed, kept between the form, the test and the save - so "change
// settings" reopens the form as it was rather than empty.
struct Form {
  String ssid[SLOTS], pass[SLOTS];
  String ntp, tz, addr, port, pat;
};
static Form form;

// ------------------------------------------------------------------- scan

struct Net {
  String ssid;
  int32_t rssi, channel;
  uint8_t bssid[6];
};
static std::vector<Net> nets;
static const size_t MAX_NETS = 25;

// The radio sees 2.4 GHz only, so a 5 GHz-only network - which the display
// could never join - is simply not in the list.
static void scan() {
  int n = WiFi.scanNetworks();
  if (n < 0) {               // -1 running, -2 failed: let it settle, once
    WiFi.scanDelete();
    delay(500);
    n = WiFi.scanNetworks();
  }
  nets.clear();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s == AP_SSID) continue;   // hidden networks, ourselves
    auto it = std::find_if(nets.begin(), nets.end(), [&](const Net &x) { return x.ssid == s; });
    if (it == nets.end()) {
      nets.push_back(Net{s, WiFi.RSSI(i), WiFi.channel(i), {}});
      it = nets.end() - 1;
    } else if (WiFi.RSSI(i) <= it->rssi) {
      continue;              // one entry per name: its strongest access point
    }
    it->rssi = WiFi.RSSI(i);
    it->channel = WiFi.channel(i);
    memcpy(it->bssid, WiFi.BSSID(i), 6);
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  if (nets.size() > MAX_NETS) nets.resize(MAX_NETS);
  Serial.printf("portal: scan found %u networks\n", (unsigned)nets.size());
}

static const Net *find_net(const String &ssid) {
  for (auto &n : nets) if (n.ssid == ssid) return &n;
  return nullptr;
}

// Lower case, letters and digits only: "Seaview12" and "seaview 12" match.
static String fold(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (isalnum((unsigned char)ch)) o += (char)tolower((unsigned char)ch);
  }
  return o;
}

static String near_miss(const String &ssid) {
  String f = fold(ssid);
  if (!f.length()) return "";
  for (auto &n : nets) if (fold(n.ssid) == f) return n.ssid;
  return "";
}

// ------------------------------------------------------------------- test

enum Verdict { V_NONE, V_NOT_SEEN, V_NO_PASS, V_OK, V_BAD_PASS, V_NO_ANSWER, V_FAILED, V_UNTESTED };
enum ProxyVerdict { P_UNTESTED, P_OK, P_NO_NAME, P_NO_ANSWER };

struct Result {
  Verdict v = V_NONE;
  int reason = 0;
  int32_t rssi = 0;
  String hint;               // the network a not-seen name probably meant
  ProxyVerdict proxy = P_UNTESTED;
  String proxy_ip;
};
static Result results[SLOTS];

enum Phase { IDLE, TESTING, TESTED };
static Phase phase = IDLE;
static int testing_slot = 0;

static volatile uint8_t disc_reason = 0;
static const uint32_t TEST_JOIN_MS = 12000;
static const uint32_t TEST_LOOKUP_MS = 6000;
static const uint32_t TEST_CONNECT_MS = 3000;

static void pump() {
  server.handleClient();
  if (use_ap) dns.processNextRequest();
}

// The proxy, looked for over the network just joined. IPv4 only, for the
// reason given at proxy_address() in the main sketch.
static ProxyVerdict proxy_check(Result &r) {
  IPAddress ip;
  if (!ip.fromString(form.addr)) {
    bool found = false;
    for (uint32_t t0 = millis(); millis() - t0 < TEST_LOOKUP_MS; ) {
      struct addrinfo hints = {};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      struct addrinfo *res = nullptr;
      if (lwip_getaddrinfo(form.addr.c_str(), "0", &hints, &res) == 0 && res) {
        ip = IPAddress(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
        lwip_freeaddrinfo(res);
        found = true;
        break;
      }
      for (int i = 0; i < 50; i++) { pump(); delay(10); }
    }
    if (!found) return P_NO_NAME;
  }
  r.proxy_ip = ip.toString();
  WiFiClient client;
  if (!client.connect(ip, (uint16_t)form.port.toInt(), TEST_CONNECT_MS)) return P_NO_ANSWER;
  client.stop();
  return P_OK;
}

// Joins one network with the given password, keeping the access point up.
// A station on another channel moves the access point with it - the radio
// has one channel - so a phone on the portal may drop off for a moment.
// The results also go to the panel for that reason.
static void test_slot(int i) {
  Result &r = results[i];
  r = Result();
  const String &ssid = form.ssid[i], &pass = form.pass[i];
  if (!ssid.length()) return;
  if (!pass.length()) { r.v = V_NO_PASS; return; }
  if (!use_ap) { r.v = V_UNTESTED; return; }   // testing would cut off the STA test session
  const Net *n = find_net(ssid);
  if (!n) {
    r.v = V_NOT_SEEN;
    r.hint = near_miss(ssid);
    return;
  }

  Serial.printf("portal: testing '%s'\n", ssid.c_str());
  WiFi.disconnect(false);
  for (int k = 0; k < 20; k++) { pump(); delay(10); }
  disc_reason = 0;
  WiFi.begin(ssid.c_str(), pass.c_str(), n->channel, n->bssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < TEST_JOIN_MS) {
    // The first refusal is the answer; waiting out the retries adds nothing.
    // 8 (ASSOC_LEAVE) is our own disconnect above, not a refusal.
    if (disc_reason && disc_reason != WIFI_REASON_ASSOC_LEAVE) break;
    pump();
    delay(20);
  }
  if (WiFi.status() == WL_CONNECTED) {
    r.v = V_OK;
    r.rssi = WiFi.RSSI();
    r.proxy = proxy_check(r);
  } else {
    r.reason = disc_reason;
    switch (disc_reason) {
      // What a wrong password produces, depending on the router and on
      // where the handshake gives up.
      case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      case WIFI_REASON_HANDSHAKE_TIMEOUT:
      case WIFI_REASON_AUTH_FAIL:
      case WIFI_REASON_AUTH_EXPIRE:
      case WIFI_REASON_MIC_FAILURE:
        r.v = V_BAD_PASS;
        break;
      case WIFI_REASON_NO_AP_FOUND:
      case 0:                // nothing at all within the time
        r.v = V_NO_ANSWER;
        break;
      default:
        r.v = V_FAILED;
    }
  }
  Serial.printf("portal: '%s' -> verdict %d, reason %d, proxy %d\n",
                ssid.c_str(), (int)r.v, r.reason, (int)r.proxy);
  WiFi.disconnect(false);
}

static void status(const char *msg);

static const char *short_verdict(const Result &r) {
  switch (r.v) {
    case V_OK:       return r.proxy == P_OK ? "OK, proxy OK" : "joined, NO proxy";
    case V_BAD_PASS: return "WRONG password";
    case V_NOT_SEEN: return "not nearby, untested";
    case V_NO_PASS:  return "NO password";
    case V_NO_ANSWER:return "no answer";
    case V_UNTESTED: return "untested";
    default:         return "FAILED";
  }
}

static void run_tests() {
  status("Testing the WiFi settings\nThis takes up to a minute.");
  for (testing_slot = 0; testing_slot < SLOTS; testing_slot++) test_slot(testing_slot);
  phase = TESTED;
  String msg = "WiFi test done";
  for (int i = 0; i < SLOTS; i++) {
    if (results[i].v == V_NONE) continue;
    msg += "\n" + form.ssid[i] + ": " + short_verdict(results[i]);
  }
  msg += "\nDetails on the setup page";
  status(msg.c_str());
}

// ------------------------------------------------------------------- pages

static String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += s[i];
    }
  }
  return o;
}

static const char ROW[] = "<tr style=\"background-color:rgb(230,230,255)\"><td>";
static const char TABLE[] =
  "<table style=\"text-align:left;width:400px;max-width:100%;background-color:white;"
  "font-weight:bold;font-size:14px\" cellpadding=2 cellspacing=3><tbody>";

static String page_head(const char *extra_head = "") {
  return String(
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>M5Ink Minimed Mon</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>") + extra_head +
    "</head><body style=\"font-family:Helvetica,Arial,sans-serif\">"
    "<table style=\"text-align:left;width:400px;max-width:100%;background-color:#2196F3;"
    "font-weight:bold;color:white\" cellpadding=2 cellspacing=2><tbody><tr><td>"
    "<span style=\"font-size:48px\">M5Ink Minimed Mon</span><br>"
    "<span style=\"font-size:20px;color:rgb(204,255,255)\">Configuration</span>"
    "</td></tr></tbody></table><br>";
}

// No auto-capitalisation, autocorrect or spellcheck: a phone turns
// "office" into "Office" on its own, and WiFi names and passwords are
// case-sensitive. Seen on a board set up from a phone: its proxy name came
// back as "Carelink-01.local" (harmless there, mDNS ignores case).
static String input(const char *name, const String &value) {
  return String("<input type='text' autocapitalize='none' autocorrect='off' spellcheck='false' name='") +
         name + "' value='" + esc(value) + "'>";
}

// A list of what is in range above each name field; picking one copies the
// exact name into the field, which stays editable for a network that is
// somewhere else (home, while setting up on holiday) or hidden.
static String network_picker(const char *field) {
  if (nets.empty()) return "";
  String h = String("<select style='max-width:100%' onchange=\"if(this.value)this.form.elements['") +
             field + "'].value=this.value\"><option value=''>Pick a network in range…</option>";
  for (auto &n : nets) {
    String e = esc(n.ssid);
    h += "<option value='" + e + "'>" + e + " (" + String(n.rssi) + " dBm)</option>";
  }
  return h + "</select><br>";
}

static String config_page(const char *error = nullptr) {
  String html = page_head();
  if (error) html += String("<p style='color:red;font-weight:bold'>") + error + "</p>";
  html += "<form action='/config' method='post'>";
  html += TABLE;
  html += "<tr style=\"font-size:18px;background-color:lightgrey\"><td>Wifi parameters</td>";
  html += String(ROW) + "<span style='font-weight:normal'>" + String(nets.size()) +
          " networks in range. Only 2.4 GHz networks are listed: the display cannot use "
          "5 GHz. <a href='/rescan'>Scan again</a></span></td>";
  for (int i = 0; i < SLOTS; i++) {
    html += String(ROW) + SSID_LABEL[i] + "<br>" + network_picker(SSID_FIELD[i]) +
            input(SSID_FIELD[i], form.ssid[i]) + "</td>";
    html += String(ROW) + PASS_LABEL[i] + "<br>" + input(PASS_FIELD[i], form.pass[i]) + "</td>";
  }
  html += "</tbody></table><br>";
  html += TABLE;
  html += "<tr style=\"font-size:18px;background-color:lightgrey\"><td>Time and date</td>";
  html += String(ROW) + "NTP server address<br>" + input("fntpserver", form.ntp) + "</td>";
  html += String(ROW) + "Time Zone (h) (1 = GMT+1 = Brussels; summer time is automatic)<br>" +
          input("ftimezone", form.tz) + "</td>";
  html += "</tbody></table><br>";
  html += TABLE;
  html += "<tr style=\"font-size:18px;background-color:lightgrey\"><td>Carelink proxy</td>";
  html += String(ROW) + "IP address<br>" + input("fproxyaddr", form.addr) + "</td>";
  html += String(ROW) + "Port<br>" + input("fproxyport", form.port) + "</td>";
  html += String(ROW) + "Patient name (optional)<br>" + input("fpatient", form.pat) + "</td>";
  html += "</tbody></table><br><input type='submit' value='Test and save'>"
          "<p style='font-size:13px;color:grey;max-width:400px'>The display tries each network "
          "and looks for the proxy before anything is stored. Your phone may drop off this "
          "setup network for a moment while it does; the result is also shown on the "
          "display.</p></form></body></html>";
  return html;
}

static String testing_page() {
  String html = page_head("<meta http-equiv='refresh' content='3;url=/result'>");
  html += TABLE;
  html += String(ROW) + "<span style='font-size:18px'>Testing the WiFi settings…</span><br>"
          "<span style='font-weight:normal'>Network " + String(min(testing_slot + 1, SLOTS)) +
          " of " + String(SLOTS) + ". This page refreshes by itself. If your phone left the "
          "setup network, join <b>" + AP_SSID + "</b> again; the display also shows the "
          "result.</span></td></tbody></table></body></html>";
  return html;
}

static String result_line(int i, bool &problem) {
  const Result &r = results[i];
  const String name = "<b>" + esc(form.ssid[i]) + "</b>";
  const bool fallback = (i == SLOTS - 1);
  const bool have_fallback = form.ssid[SLOTS - 1].length() > 0;
  String s;
  switch (r.v) {
    case V_OK:
      s = "&#10003; " + name + ": password accepted (" + String(r.rssi) + " dBm).<br>";
      if (r.proxy == P_OK) {
        s += "&#10003; The proxy answered over this network.";
      } else {
        problem = true;
        s += r.proxy == P_NO_NAME
          ? "&#10007; The proxy <b>" + esc(form.addr) + "</b> was not found over this network. Check its name. "
          : "&#10007; The proxy was found (" + r.proxy_ip + ") but did not answer on port " + esc(form.port) + ". ";
        if (fallback) s += "The fallback would not work.";
        else if (have_fallback) s += "Here the display would use the fallback instead; this network may keep its devices apart.";
        else s += "This network may keep its devices apart.";
      }
      break;
    case V_BAD_PASS:
      problem = true;
      s = "&#10007; " + name + ": the password was <b>rejected</b>.";
      break;
    case V_NO_ANSWER:
      problem = true;
      s = "&#10007; " + name + ": did not answer, although it was in range a moment ago.";
      break;
    case V_FAILED:
      problem = true;
      s = "&#10007; " + name + ": could not connect (reason " + String(r.reason) + ").";
      break;
    case V_NO_PASS:
      problem = true;
      s = "&#10007; " + name + ": no password. The display ignores a network without one.";
      break;
    case V_NOT_SEEN:
      problem = true;
      s = "&#9888; " + name + " is not in range, so it was not tested.";
      if (r.hint.length())
        s += " Did you mean <b>" + esc(r.hint) + "</b>? Names must match exactly, spaces and capitals included.";
      else
        s += " Check the spelling, unless this network is somewhere else.";
      break;
    case V_UNTESTED:
      s = "&#8226; " + name + ": not tested (portal test over an existing connection).";
      break;
    default:
      return "";
  }
  return String(ROW) + "<span style='font-weight:normal'>" + s + "</span></td>";
}

static String result_page() {
  String html = page_head();
  html += TABLE;
  html += "<tr style=\"font-size:18px;background-color:lightgrey\"><td>WiFi test</td>";
  bool problem = false;
  for (int i = 0; i < SLOTS; i++) html += result_line(i, problem);
  html += "</tbody></table><br>";
  html += problem
    ? "<p style='max-width:400px'>Something above needs a look. Change the settings, or save "
      "them as they are if a warning is expected (a network that is somewhere else).</p>"
    : "<p style='max-width:400px'>All good.</p>";
  html += "<form action='/save' method='post' style='display:inline'>"
          "<input type='submit' value='" + String(problem ? "Save anyway" : "Save these settings") +
          "'></form> &nbsp; <a href='/'>Change settings</a>";
  if (dry_run) html += "<p style='color:grey'>Test build: saving stores nothing.</p>";
  html += "</body></html>";
  return html;
}

static const char SUCCESS_BODY[] =
  "<table style=\"text-align:left;width:400px;max-width:100%;background-color:rgb(230,230,255);"
  "font-weight:bold;font-size:14px\" cellpadding=2 cellspacing=3><tbody>"
  "<tr><td style=\"color:green;font-size:18px\">Parameters updated successfully</td>"
  "<tr><td style=\"color:grey\">Restarting device with new configuration ...</td>"
  "</tbody></table></body></html>";

// ---------------------------------------------------------------- panel

// Silent by design, even though this board HAS a buzzer: AP mode means
// somebody is standing in front of the device doing setup, so there is
// nobody to summon. The buzzer is reserved for pump alarms - if it also
// chirped for routine setup steps it would train the caregiver to ignore it,
// which is the one failure it exists to prevent.
//
// Full refresh: these are rare, discrete transitions, and a clean panel
// matters more than speed when someone is reading instructions off it.
static void status(const char *msg) {
  Serial.printf("portal: %s\n", msg);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  M5Canvas canvas(&M5.Display);
  canvas.setColorDepth(CANVAS_BPP);
  if (!canvas.createSprite(PANEL_W, PANEL_H)) {
    draw_status_screen(M5.Display, msg);
    return;
  }
  draw_status_screen(canvas, msg);
  canvas.pushSprite(0, 0);
  canvas.deleteSprite();
}

// A fresh access-point password every time the portal starts, shown on the
// panel. The fixed one this replaced was printed in the repo, and with WPA2
// knowing the password is enough to decrypt a recorded session - including
// the home WiFi password typed into this form. Someone standing in front of
// the panel can read it; someone listening from next door cannot.
//
// 12 characters from 31 that cannot be misread (no 0/o, 1/l/i): about 59
// bits, far beyond what an offline attack on a recorded handshake can reach.
// Must be called with the radio ON: esp_random() is only a true random
// source while WiFi or Bluetooth is running.
static String random_ap_pass() {
  static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  const uint32_t n = sizeof(ALPHABET) - 1;
  String out;
  while (out.length() < 12) {
    uint8_t b = esp_random() & 0xFF;
    if (b >= 256 - (256 % n)) continue;   // reject, so every character is equally likely
    out += ALPHABET[b % n];
  }
  return out;
}

// The join screen: a WiFi QR code a phone camera understands, plus the same
// details as text for a laptop. Replaced by the "connection established"
// status as soon as a client joins, so the password does not stay on the
// panel any longer than it is needed.
static void show_join(const char *ssid, const String &pass) {
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  M5Canvas canvas(&M5.Display);
  canvas.setColorDepth(CANVAS_BPP);
  if (!canvas.createSprite(PANEL_W, PANEL_H)) {
    draw_join_screen(M5.Display, ssid, pass.c_str());
    return;
  }
  draw_join_screen(canvas, ssid, pass.c_str());
  canvas.pushSprite(0, 0);
  canvas.deleteSprite();
}

// ---------------------------------------------------------------- serve

static void store() {
  if (dry_run) {
    Serial.println("portal: test build, nothing stored");
    return;
  }
  Preferences p;
  if (!p.begin("minimed", false)) return;
  p.putString("wifissid",  form.ssid[0]);
  p.putString("wifipass",  form.pass[0]);
  // Optional slots: absent values simply clear them.
  p.putString("ssid1",     form.ssid[1]);
  p.putString("pass1",     form.pass[1]);
  p.putString("ssid2",     form.ssid[2]);
  p.putString("pass2",     form.pass[2]);
  p.putString("proxyaddr", form.addr);
  p.putString("ntpserver", form.ntp);
  p.putString("patient",   form.pat);
  p.putUShort("proxyport", (uint16_t)form.port.toInt());
  p.putInt("timezone",     form.tz.toInt());
  p.end();
  Serial.println("portal: configuration stored");
}

static void serve(const Config &c) {
  form = Form();
  form.ntp  = c.ntpserver.length() ? c.ntpserver : String("pool.ntp.org");
  form.tz   = String(c.timezone);
  form.port = String(c.proxyport);

  // AP+STA from the start: the station half scans now and tests later, while
  // the access point keeps serving the form.
  if (use_ap) WiFi.mode(WIFI_AP_STA);         // radio on first: see random_ap_pass()
  WiFi.setAutoReconnect(false);               // a refused test must not keep retrying
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    disc_reason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // Scanned BEFORE the access point is up: a client already on it would
  // otherwise lose it while the radio hops channels.
  scan();

  if (use_ap) {
    ap_pass = random_ap_pass();
    WiFi.softAP(AP_SSID, ap_pass.c_str(), 1, 0, 1);   // channel 1, not hidden, 1 client
    // The password is deliberately not logged: the panel is where it belongs.
    // A test build is the exception - it has no one in front of the panel.
    if (dry_run) Serial.printf("portal: TEST BUILD ap password %s\n", ap_pass.c_str());
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());
    Serial.printf("portal: AP %s at %s\n", AP_SSID, AP_ADDR);
    show_join(AP_SSID, ap_pass);
  } else {
    Serial.printf("portal: serving on http://%s/ (STA test)\n",
                  WiFi.localIP().toString().c_str());
  }

  server.on("/", []() {
    if (phase == TESTING) { server.send(200, "text/html", testing_page()); return; }
    server.send(200, "text/html", config_page());
  });

  server.on("/rescan", []() {
    if (phase != TESTING) scan();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/config", []() {
    if (phase == TESTING) { server.send(200, "text/html", testing_page()); return; }
    // WebServer::arg() URL-decodes, which the MicroPython build's raw string
    // splitting does not - so a password containing %, & or a space works
    // here and silently does not there.
    for (int i = 0; i < SLOTS; i++) {
      form.ssid[i] = server.arg(SSID_FIELD[i]);
      form.pass[i] = server.arg(PASS_FIELD[i]);
    }
    form.ntp  = server.arg("fntpserver");
    form.tz   = server.arg("ftimezone");
    form.addr = server.arg("fproxyaddr");
    form.port = server.arg("fproxyport");
    form.pat  = server.arg("fpatient");
    // Patient name is deliberately NOT required: the proxy reports
    // firstName, so a device never told a name still shows the right one.
    if (!form.ssid[0].length() || !form.pass[0].length() || !form.ntp.length() ||
        !form.tz.length() || !form.addr.length() || !form.port.length()) {
      server.send(200, "text/html", config_page(
        "Fill in at least the first network and its password, the NTP server, the time zone, "
        "and the proxy address and port."));
      return;
    }
    phase = TESTING;
    testing_slot = 0;
    server.send(200, "text/html", testing_page());
  });

  server.on("/result", []() {
    if (phase == TESTING) server.send(200, "text/html", testing_page());
    else if (phase == TESTED) server.send(200, "text/html", result_page());
    else { server.sendHeader("Location", "/"); server.send(303); }
  });

  server.on("/save", []() {
    if (phase != TESTED) { server.sendHeader("Location", "/"); server.send(303); return; }
    store();
    server.send(200, "text/html", page_head() + SUCCESS_BODY);
    saved = true;
  });

  server.onNotFound([]() {
    if (phase == TESTING) { server.send(200, "text/html", testing_page()); return; }
    server.send(200, "text/html", config_page());
  });
  server.begin();

  // Blocks until configured. No timeout on purpose: there is nothing useful
  // to fall back to - a device with no proxy address cannot monitor
  // anything - and someone is present by definition.
  bool announced = false;
  char msg[160];
  while (!saved) {
    pump();
    if (phase == TESTING) run_tests();
    if (use_ap && !announced && WiFi.softAPgetStationNum() > 0) {
      announced = true;
      snprintf(msg, sizeof(msg),
               "WIFI connection established\nThe setup page should open by itself. If not, browse to http://%s",
               AP_ADDR);
      status(msg);
    }
    delay(5);
  }

  // Let the success page reach the browser before the radio goes down.
  for (int i = 0; i < 100; i++) { server.handleClient(); delay(10); }

  status(dry_run ? "Test build: nothing stored\nResetting device ..."
                 : "New configuration parameters stored\nResetting device ...");
  delay(2000);
  esp_restart();
}

void run_config_portal(const Config &current, bool test_build) {
  use_ap = true;
  dry_run = test_build;
  serve(current);
}

void run_config_portal_sta(const Config &current) {
  use_ap = false;
  dry_run = false;
  serve(current);
}
