// One-shot NVS editor for the minimed Core Ink: read or change one WiFi slot
// without rewriting the whole NVS image, so no other credential is ever
// retyped. Flash this, drive it with nvs_talk.py, then reflash
// minimed-coreink - an app upload does not touch NVS. See README.md.
//
// Serial protocol (115200, one line per command):
//   DUMP                      -> slots (SSID + password LENGTH only), proxy
//   SET <n>\t<ssid>\t<pass>   -> makes slot n exactly (ssid, pass)
//   BACKUP <n>                -> copies slot n to ssidbak/passbak (keys the
//                                firmware never reads), only if empty
//   RESTORE <n>               -> copies the backup into slot n
//   RENAME <n>\t<ssid>        -> new SSID for slot n, password kept
//   CLEAR <n>                 -> empties slot 1 or 2 (slot 0 is required)
// Every command is idempotent, keyed on the end state: an upload ends with a
// hard reset and attaching a console resets again, so setup() can run more
// than once. Passwords are never printed. A missing input is refused rather
// than half-written. Slot 2 is the fallback (plan B), the Pi's hotspot.
#include <Preferences.h>
#include <driver/gpio.h>

__attribute__((constructor)) static void early_power_hold() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << GPIO_NUM_12;
  cfg.mode = GPIO_MODE_OUTPUT;
  gpio_config(&cfg);
  gpio_set_level(GPIO_NUM_12, 1);
}

static const char *SSID_KEY[3] = {"wifissid", "ssid1", "ssid2"};
static const char *PASS_KEY[3] = {"wifipass", "pass1", "pass2"};

static void dump() {
  Preferences p;
  if (!p.begin("minimed", true)) { Serial.println("ERR no namespace"); return; }
  for (int i = 0; i < 3; i++)
    Serial.printf("slot %d: ssid='%s' passlen=%u\n", i,
                  p.getString(SSID_KEY[i], "").c_str(),
                  (unsigned)p.getString(PASS_KEY[i], "").length());
  Serial.printf("proxyaddr='%s' proxyport=%u\n",
                p.getString("proxyaddr", "").c_str(), p.getUShort("proxyport", 8081));
  p.end();
  Serial.println("END");
}

static void set_slot(const String &line) {
  int t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1);
  if (t1 < 0 || t2 < 0) { Serial.println("ERR format"); return; }
  int n = line.substring(4, t1).toInt();
  String ssid = line.substring(t1 + 1, t2), pass = line.substring(t2 + 1);
  pass.trim(); ssid.trim();
  if (n < 0 || n > 2 || !ssid.length() || pass.length() < 8) {  // refuse, never half-write
    Serial.println("ERR refused: bad slot or empty ssid/short pass");
    return;
  }
  Preferences p;
  if (!p.begin("minimed", false)) { Serial.println("ERR open rw"); return; }
  bool same = p.getString(SSID_KEY[n], "") == ssid && p.getString(PASS_KEY[n], "") == pass;
  if (!same) { p.putString(SSID_KEY[n], ssid); p.putString(PASS_KEY[n], pass); }
  p.end();
  Serial.printf("OK slot %d %s\n", n, same ? "already set" : "written");
  dump();
}

// Copies slot n into ssidbak/passbak (keys the firmware never reads), but only
// if the backup is empty - so re-running after SET never clobbers it.
static void backup_slot(int n) {
  if (n < 0 || n > 2) { Serial.println("ERR bad slot"); return; }
  Preferences p;
  if (!p.begin("minimed", false)) { Serial.println("ERR open rw"); return; }
  if (p.getString("ssidbak", "").length()) {
    Serial.printf("OK backup already holds '%s'\n", p.getString("ssidbak", "").c_str());
  } else if (!p.getString(SSID_KEY[n], "").length()) {
    Serial.println("OK slot empty, nothing to back up");
  } else {
    p.putString("ssidbak", p.getString(SSID_KEY[n], ""));
    p.putString("passbak", p.getString(PASS_KEY[n], ""));
    Serial.printf("OK backed up '%s'\n", p.getString("ssidbak", "").c_str());
  }
  p.end();
  Serial.println("END");
}

// Copies ssidbak/passbak into slot n. Idempotent: the end state is "slot n
// holds the backup", whatever it held before.
static void restore_slot(int n) {
  if (n < 0 || n > 2) { Serial.println("ERR bad slot"); return; }
  Preferences p;
  if (!p.begin("minimed", false)) { Serial.println("ERR open rw"); return; }
  String s = p.getString("ssidbak", ""), pw = p.getString("passbak", "");
  if (!s.length() || pw.length() < 8) { p.end(); Serial.println("ERR no usable backup"); return; }
  bool same = p.getString(SSID_KEY[n], "") == s && p.getString(PASS_KEY[n], "") == pw;
  if (!same) { p.putString(SSID_KEY[n], s); p.putString(PASS_KEY[n], pw); }
  p.end();
  Serial.printf("OK slot %d %s from backup\n", n, same ? "already restored" : "restored");
  dump();
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(500);
  delay(300);
  Serial.println("READY nvs-slot-edit");
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "DUMP") dump();
  else if (line.startsWith("SET ")) set_slot(line);
  else if (line.startsWith("BACKUP ")) backup_slot(line.substring(7).toInt());
  else if (line.startsWith("RESTORE ")) restore_slot(line.substring(8).toInt());
  else if (line.startsWith("CLEAR ")) {    // slot 0 is required by the firmware: never cleared
    int n = line.substring(6).toInt();
    Preferences p;
    if (n < 1 || n > 2 || !p.begin("minimed", false)) { Serial.println("ERR clear: slot 1 or 2 only"); return; }
    p.remove(SSID_KEY[n]);
    p.remove(PASS_KEY[n]);
    p.end();
    Serial.printf("OK slot %d cleared\n", n);
    dump();
  }
  else if (line.startsWith("RENAME ")) {   // RENAME <n>\t<ssid>: new SSID, password kept
    int t = line.indexOf('\t');
    int n = line.substring(7, t).toInt();
    String ssid = t > 0 ? line.substring(t + 1) : "";
    Preferences p;
    if (n < 0 || n > 2 || !ssid.length() || !p.begin("minimed", false)) { Serial.println("ERR rename"); return; }
    if (p.getString(SSID_KEY[n], "") != ssid) p.putString(SSID_KEY[n], ssid);
    p.end();
    Serial.printf("OK slot %d is '%s'\n", n, ssid.c_str());
    dump();
  }
  else if (line.length()) Serial.println("ERR unknown");
}
