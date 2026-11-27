#include "esp_mac.h"
#include "screens.h"
#include <time.h>
#include <math.h>

// ------------------------------------------------------------------- fonts
//
// NOTE the fonts are chosen differently from the MicroPython build, because
// M5GFX reports different metrics to C++ than its MicroPython binding does.
// Measured here: DejaVu72 75px tall / 141px wide for "160", DejaVu40 at 2x
// 84px tall / 156px wide, DejaVu24 25, DejaVu18 18, DejaVu12 13. The Python
// file picked DejaVu40-at-2x for the large reading precisely because ITS
// DejaVu72 measured only 50px; here DejaVu72 is both taller and 15px
// narrower, so it wins twice - and DejaVu40-at-2x would leave only ~30px
// beside a 3-digit reading, shrinking the trend arrows to 10px and losing
// the legibility the layout exists to protect.
static const lgfx::IFont *FONT_GLUCOSE     = &fonts::DejaVu40;   // 42px, alarm layout
static const lgfx::IFont *FONT_GLUCOSE_BIG = &fonts::DejaVu72;   // 75px, normal layout
static const float        GLUCOSE_BIG_SIZE = 1.0f;
static const lgfx::IFont *FONT_UNIT        = &fonts::DejaVu18;   // 20px
static const lgfx::IFont *FONT_VALUE       = &fonts::DejaVu24;   // 26px
static const lgfx::IFont *FONT_LABEL       = &fonts::DejaVu12;   // 16px
static const lgfx::IFont *FONT_ALARM_L     = &fonts::DejaVu18;   // short alarms
static const lgfx::IFont *FONT_ALARM_S     = &fonts::DejaVu12;   // long alarms, in full

// ----------------------------------------------------------------- helpers

static int text_width(LovyanGFX &g, const char *s, const lgfx::IFont *font, float size = 1.0f) {
  g.setFont(font);
  g.setTextSize(size);
  int w = g.textWidth(s);
  g.setTextSize(1.0f);
  return w;
}

static int font_height(LovyanGFX &g, const lgfx::IFont *font, float size = 1.0f) {
  g.setFont(font);
  g.setTextSize(size);
  int h = g.fontHeight();
  g.setTextSize(1.0f);
  return h;
}

static void draw_text(LovyanGFX &g, const char *s, int x, int y,
                      const lgfx::IFont *font, uint32_t color,
                      uint32_t bg = COLOR_WHITE, float size = 1.0f) {
  g.setFont(font);
  g.setTextSize(size);
  g.setTextColor(color, bg);
  g.drawString(s, x, y);
  g.setTextSize(1.0f);
}

// Bold by drawing twice, one pixel apart. The DejaVu fonts here have no bold
// face, and a different font would change the metrics every layout below is
// built on. Transparent background on both passes, or the second would erase
// half of the first.
static void draw_text_bold(LovyanGFX &g, const char *s, int x, int y,
                           const lgfx::IFont *font, uint32_t color) {
  g.setFont(font);
  g.setTextSize(1.0f);
  g.setTextColor(color);
  g.drawString(s, x, y);
  g.drawString(s, x + 1, y);
}

// Label left, value right-aligned to the right margin, bottoms aligned so a
// small label sits on the same baseline as a larger value. The value is
// placed first and the label truncated to what is left, because if something
// has to be clipped it should be the constant label, not the number that
// actually changes. Returns the value's height.
// Cuts text to max_w pixels, ending in "..." when it had to cut.
static String fit_text(LovyanGFX &g, const String &text, const lgfx::IFont *font, int max_w) {
  if (text_width(g, text.c_str(), font) <= max_w) return text;
  int ell = text_width(g, "...", font);
  String out;
  for (unsigned i = 0; i < text.length(); i++) {
    String cand = out + text[i];
    if (text_width(g, cand.c_str(), font) + ell > max_w) break;
    out = cand;
  }
  return out + "...";
}

static int draw_kv_row(LovyanGFX &g, int y,
                       const char *label, const lgfx::IFont *label_font,
                       const char *value, const lgfx::IFont *value_font,
                       uint32_t color, uint32_t bg = COLOR_WHITE) {
  int vh = font_height(g, value_font);
  int lh = font_height(g, label_font);
  int value_w = text_width(g, value, value_font);
  int value_x = PANEL_W - MARGIN - value_w;
  draw_text(g, value, value_x, y, value_font, color, bg);

  int label_max_w = value_x - MARGIN - 4;
  String lab(label);
  if (text_width(g, lab.c_str(), label_font) > label_max_w) {
    int ell = text_width(g, "...", label_font);
    String out;
    for (unsigned i = 0; i < lab.length(); i++) {
      String cand = out + lab[i];
      if (text_width(g, cand.c_str(), label_font) + ell > label_max_w) break;
      out = cand;
    }
    lab = out + "...";
  }
  draw_text(g, lab.c_str(), MARGIN, y + (vh - lh), label_font, color, bg);
  return vh;
}

// M5GFX has no auto-wrap and clips silently, so long text must be broken up
// by hand. Measures with real font metrics, these fonts being proportional.
static int wrap_text(LovyanGFX &g, const char *s, int max_w,
                     const lgfx::IFont *font, String *lines, int max_lines) {
  int n = 0;
  String cur;
  String rest(s);
  int start = 0;
  while (start <= (int)rest.length() && n < max_lines) {
    int sp = rest.indexOf(' ', start);
    String word = (sp < 0) ? rest.substring(start) : rest.substring(start, sp);
    String cand = cur.length() ? cur + " " + word : word;
    if (cur.length() && text_width(g, cand.c_str(), font) > max_w) {
      lines[n++] = cur;
      cur = word;
    } else {
      cur = cand;
    }
    if (sp < 0) break;
    start = sp + 1;
  }
  if (cur.length() && n < max_lines) lines[n++] = cur;
  return n;
}

static String truncate_to_width(LovyanGFX &g, const String &s, int max_w,
                                const lgfx::IFont *font) {
  if (text_width(g, s.c_str(), font) <= max_w) return s;
  int ell = text_width(g, "...", font);
  String out;
  for (unsigned i = 0; i < s.length(); i++) {
    String cand = out + s[i];
    if (text_width(g, cand.c_str(), font) + ell > max_w) break;
    out = cand;
  }
  return out + "...";
}

// Real arrow shapes rather than caret characters: M5GFX gives fillTriangle,
// so the head is one call and the shaft another. main.py had to print
// glyphs because the Inkplate driver could only do that; here the trend is
// what turns a number into a direction, so it is drawn properly.
enum ArrowDir { ARROW_UP, ARROW_FLAT, ARROW_DOWN };

static void draw_arrows(LovyanGFX &g, int x, int y, int w, int h,
                        ArrowDir dir, int count, uint32_t color) {
  for (int i = 0; i < count; i++) {
    int ax = x + i * (w + 3);
    int cx = ax + w / 2;
    int shaft_w = max(3, w / 3);
    if (dir == ARROW_FLAT) {
      int head_w = w / 2;
      int cy = y + h / 2;
      g.fillRect(ax, cy - shaft_w / 2, w - head_w, shaft_w, color);
      g.fillTriangle(ax + w - head_w, cy - h / 4,
                     ax + w - head_w, cy + h / 4,
                     ax + w,          cy, color);
    } else if (dir == ARROW_UP) {
      int head_h = h / 2;
      g.fillTriangle(ax, y + head_h, ax + w, y + head_h, cx, y, color);
      g.fillRect(cx - shaft_w / 2, y + head_h, shaft_w, h - head_h, color);
    } else {
      int head_h = h / 2;
      g.fillTriangle(ax, y + h - head_h, ax + w, y + h - head_h, cx, y + h, color);
      g.fillRect(cx - shaft_w / 2, y, shaft_w, h - head_h, color);
    }
  }
}

static void trend_arrows(const char *trend, ArrowDir &dir, int &count) {
  struct { const char *name; ArrowDir dir; int count; } table[] = {
    {"UP_TRIPLE",   ARROW_UP,   3},
    {"UP_DOUBLE",   ARROW_UP,   2},
    {"UP",          ARROW_UP,   1},
    {"NONE",        ARROW_FLAT, 1},
    {"DOWN",        ARROW_DOWN, 1},
    {"DOWN_DOUBLE", ARROW_DOWN, 2},
    {"DOWN_TRIPLE", ARROW_DOWN, 3},
  };
  for (auto &e : table) {
    if (!strcmp(trend, e.name)) { dir = e.dir; count = e.count; return; }
  }
  dir = ARROW_FLAT;
  count = 1;
}

// ------------------------------------------------------------------- time

static void local_tm(time_t utc, int tz_hours, int dst, struct tm &out) {
  time_t t = utc + (time_t)(tz_hours + dst) * 3600;
  gmtime_r(&t, &out);
}

// Same rule as main.py: minute/hour fields only, and anything past 15
// minutes or an hour is reported as no data rather than a large number.
// How old the reading is, from the two epochs rather than from clock fields.
// The field version this replaced compared hours and minutes separately, so a
// reading from 09:33 shown at 10:38 read "5 min ago", and past 15 minutes it
// said "No data" although there was data, just old. A proxy stuck on one
// reading for hours looked almost normal (carelink-01, 2026-10-31).
static void age_txt(long age_s, char *out, size_t n) {
  long m = age_s / 60;
  if (m < 1)              snprintf(out, n, "now");
  else if (m < 60)        snprintf(out, n, "%ld min", m);
  else if (m < 24 * 60)   snprintf(out, n, "%ld h %ld min", m / 60, m % 60);
  else if (m < 48 * 60)   snprintf(out, n, "1 day");
  else                    snprintf(out, n, "%ld days", m / (24 * 60));
}

// Past this the reading is called out in the banner, not left to the small
// Updated line. The pump uploads every 5 minutes and the display lags it by
// up to two cycles, so 20 minutes is well past normal and still short of
// anything a caregiver would want to discover by accident.
static const long STALE_READING_S = 20 * 60;

static long reading_age_s(const State &s) {
  time_t now = time(nullptr);
  if (!s.lastUpdate || now < 1600000000) return -1;   // no reading, or no clock
  long age = (long)(now - s.lastUpdate);
  return age < 0 ? 0 : age;
}

// The device battery, as a battery symbol: an outlined body with a nub on the
// right, filled in three steps.
//
// Deliberately small. It competes for attention with the one number this
// device exists to show, and it is housekeeping - a glance should find the
// glucose reading first and this second. Returns the width drawn, nub
// included, so the caller can right-align it.
static const int BATT_W = 26, BATT_H = 13, BATT_NUB_W = 3, BATT_NUB_H = 5;

// A median of several reads, because _getBatteryAdcRaw() is a single-shot
// conversion with no averaging: this is a bare ADC on GPIO35 behind a
// 25.1k/5.1k divider, no fuel gauge. Measured on this board, a resting pack
// spreads 4276-4306mV with a 4.9mV stdev, so it is repeatable to about
// +-15mV even though absolute accuracy is worth perhaps +-50-100mV.
//
// Called at DRAW time, which on both paths is after the radio is down. That
// matters: WiFi transmit bursts sag the rail, and a reading taken mid-fetch
// reads low enough to drop the icon a whole step.
int battery_mv() {
  int s[5];
  for (int i = 0; i < 5; i++) {
    s[i] = (int)M5.Power.getBatteryVoltage();
    delay(2);
  }
  for (int i = 1; i < 5; i++) {          // insertion sort, five elements
    int v = s[i], j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  return s[2];
}

// Which of the three steps to draw, given the previous step so the boundary
// is sticky. prev < 0 means no history (cold boot): decide on the bare
// thresholds rather than biasing the first reading either way.
int battery_state(int mv, int prev) {
  int full = BATTERY_FULL_MV + (prev >= 0 && prev < BATT_FULL ? BATTERY_HYST_MV : 0);
  int half = BATTERY_HALF_MV + (prev >= 0 && prev < BATT_HALF ? BATTERY_HYST_MV : 0);
  if (mv >= full) return BATT_FULL;
  if (mv >= half) return BATT_HALF;
  return BATT_EMPTY;
}

static int draw_battery_icon(LovyanGFX &g, int x, int y, int state) {
  g.drawRect(x, y, BATT_W, BATT_H, COLOR_BLACK);
  // Nub on the right, vertically centred against the body.
  g.fillRect(x + BATT_W, y + (BATT_H - BATT_NUB_H) / 2,
             BATT_NUB_W, BATT_NUB_H, COLOR_BLACK);

  // Inset by 2 so the fill never touches the 1px border, which would make a
  // full cell and a half cell hard to tell apart on e-paper.
  int inner_x = x + 2, inner_y = y + 2;
  int inner_w = BATT_W - 4, inner_h = BATT_H - 4;
  if (state == BATT_FULL) {
    g.fillRect(inner_x, inner_y, inner_w, inner_h, COLOR_BLACK);
  } else if (state == BATT_HALF) {
    g.fillRect(inner_x, inner_y, inner_w / 2, inner_h, COLOR_BLACK);
  }
  return BATT_W + BATT_NUB_W;
}

// What to say instead of leaving a bare "---" to mean four different things.
//
// Worded for whoever is standing in front of the panel rather than for the
// person who will fix it: the caregiver needs to know whether this is theirs
// to worry about. "No sensor data" is routine and clears itself; the other
// three do not, and someone has to act.
static const char *link_text(int link) {
  switch (link) {
    case LINK_NO_SENSOR:     return "No sensor data";
    case LINK_PROXY_NO_DATA: return "Proxy has no data";
    case LINK_PROXY_LOGIN:   return "CareLink login needed";
    case LINK_NO_PROXY:      return "No link to proxy";
    // The two the display has on its own. Both point at the screen that
    // shows which network it tried and how strong it was.
    case LINK_NO_WIFI:       return "No WiFi - see Network";
    case LINK_ABORTED:       return "Update failed - see Network";
    default:                 return "";
  }
}

// ------------------------------------------------------------- main screen

void draw_main_screen(LovyanGFX &g, const State &s, const Config &c) {
  g.fillScreen(COLOR_WHITE);

  // An alarm outranks everything; a link fault outranks the generic pump
  // banner, because when the link is down that banner is as stale as the
  // reading and saying so is more use than repeating it.
  //
  // One exception: with no reading but the pump in touch with the phone, the
  // pump's own banner (DELIVERY SUSPEND when it is disconnected, for one)
  // outranks "No sensor data". It is current - the pump just sent it - and it
  // is the reason there is no reading, which "No sensor data" only restates.
  char banner_buf[110];
  const bool pump_says_why = s.link == LINK_NO_SENSOR && s.banner[0] && s.pumpInRange == 1;
  if (s.alarm_text[0]) {
    if (s.alarm_local) {
      // For a genuine alarm (not the generic banner, which carries no
      // comparable timestamp) append when it actually occurred - that is how
      // a caregiver tells a fresh alarm from one still showing because it is
      // within its re-announce window, and a repeat beep from a new one.
      struct tm at;
      gmtime_r(&s.alarm_local, &at);   // already local wall clock
      snprintf(banner_buf, sizeof(banner_buf), "%s (%02d:%02d)",
               s.alarm_text, at.tm_hour, at.tm_min);
    } else {
      snprintf(banner_buf, sizeof(banner_buf), "%s", s.alarm_text);
    }
  } else if (pump_says_why) {
    snprintf(banner_buf, sizeof(banner_buf), "%s", s.banner);
  } else if (s.link == LINK_NO_SENSOR && s.lastGoodAt && time(nullptr) >= 1600000000) {
    // Since when there has been no glucose value: the time of the last
    // reading, so a few minutes reads as routine and hours do not. Not the
    // age of the last upload, which this showed before: the phone keeps
    // uploading without a glucose value through a sensor gap, so that age
    // stayed under 5 minutes however long the gap lasted, and read as
    // "missing for 3 min" after 15 and more. Past a day a clock time is
    // ambiguous, so it says how long instead.
    long gap = (long)(time(nullptr) - s.lastGoodAt);
    if (gap < 24 * 3600) {
      struct tm at;
      local_tm(s.lastGoodAt, c.timezone, s.dstDelta, at);
      snprintf(banner_buf, sizeof(banner_buf), "%s since %02d:%02d",
               link_text(s.link), at.tm_hour, at.tm_min);
    } else {
      char age[24];
      age_txt(gap, age, sizeof(age));
      snprintf(banner_buf, sizeof(banner_buf), "%s for %s", link_text(s.link), age);
    }
  } else if (s.link != LINK_OK) {
    snprintf(banner_buf, sizeof(banner_buf), "%s", link_text(s.link));
  } else if (reading_age_s(s) >= STALE_READING_S) {
    // Everything answers, but the reading does not move: phone out of range
    // of the pump, phone offline, or a proxy stuck on one fetch. Outranks the
    // pump banner, which is exactly as old as the reading.
    char age[24];
    age_txt(reading_age_s(s), age, sizeof(age));
    snprintf(banner_buf, sizeof(banner_buf), "No new reading for %s", age);
  } else {
    snprintf(banner_buf, sizeof(banner_buf), "%s", s.banner);
  }
  const char *banner_src = banner_buf;
  bool has_banner = banner_src[0] != '\0';

  // The reading is drawn large whenever the bottom strip is not needed for
  // an alarm, and shrinks only to make room for one: nothing else on this
  // screen competes for attention at a distance, so there is no reason to
  // render the one number the device exists for at half the available size.
  // When an alarm IS showing the trade reverses - the message matters more
  // than another 40px of digit, and the smaller figure is still legible.
  char sg_txt[8];
  if (s.sg > 0) snprintf(sg_txt, sizeof(sg_txt), "%d", s.sg);
  else          snprintf(sg_txt, sizeof(sg_txt), "---");
  bool out_of_range = s.sg > 0 &&
                      (s.sg > HYPER_THRESHOLD_MGDL || s.sg < HYPO_THRESHOLD_MGDL);

  const lgfx::IFont *g_font = has_banner ? FONT_GLUCOSE : FONT_GLUCOSE_BIG;
  float g_size = has_banner ? 1.0f : GLUCOSE_BIG_SIZE;

  int gh = font_height(g, g_font, g_size);
  int gy = 6;
  int band_y = gy - 4;
  int band_h = gh + 8;

  // No red ink here (the Inkplate coloured this number red), so out-of-range
  // inverts the whole glucose band. Full panel width rather than hugging the
  // digits, so it reads as a solid alert block from across a room.
  uint32_t fg = COLOR_BLACK, bg = COLOR_WHITE;
  if (out_of_range) {
    g.fillRect(0, band_y, PANEL_W, band_h, COLOR_BLACK);
    fg = COLOR_WHITE;
    bg = COLOR_BLACK;
  }

  draw_text(g, sg_txt, MARGIN, gy, g_font, fg, bg, g_size);

  int arrow_x = MARGIN + text_width(g, sg_txt, g_font, g_size) + 10;
  ArrowDir dir; int count;
  trend_arrows(s.trend, dir, count);
  int avail = PANEL_W - MARGIN - arrow_x;
  int arrow_w = 22;
  // Shrink rather than let a 3-arrow trend beside a 3-digit reading run off
  // the right edge, which M5GFX would clip silently.
  while (count * (arrow_w + 3) > avail && arrow_w > 7) arrow_w--;
#if DEBUG_LAYOUT
  Serial.printf("arrows: sg_w=%d arrow_x=%d avail=%d count=%d w=%d  (band %d..%d)\n",
                text_width(g, sg_txt, g_font, g_size), arrow_x, avail, count,
                arrow_w, band_y, band_y + band_h);
#endif
  draw_arrows(g, arrow_x, gy + 4, arrow_w, gh - 8, dir, count, fg);

  int unit_y = band_y + band_h + 2;
  draw_text(g, "mg/dL", MARGIN, unit_y, FONT_UNIT, COLOR_BLACK);

  // While the reading is "---", show what it last WAS, bracketed and right
  // aligned on the unit line. The brackets are doing real work: they mark the
  // number as historical, so it can never be mistaken at a glance for a
  // current reading. Shown whenever there is no reading, independently of the
  // "No sensor data" banner, which only appears once the gap has repeated.
  if (s.sg <= 0 && s.lastGoodSg > 0) {
    char prev[12];
    snprintf(prev, sizeof(prev), "[%d]", s.lastGoodSg);
    int pw = text_width(g, prev, FONT_UNIT);
    draw_text(g, prev, PANEL_W - MARGIN - pw, unit_y, FONT_UNIT, COLOR_BLACK);
  }

  int sep_y = unit_y + font_height(g, FONT_UNIT) + 6;
  g.drawLine(MARGIN, sep_y, PANEL_W - MARGIN, sep_y, COLOR_BLACK);

  // With no alarm the bottom strip is free, so these two rows sit against
  // the bottom edge rather than floating under the separator - balancing the
  // taller figure above, and keeping "Updated" in the same place it occupies
  // on the alarm layout so the eye does not have to hunt for it.
  int row_a_h = font_height(g, FONT_VALUE);
  int row_b_h = font_height(g, FONT_LABEL);
  int row_a_y = has_banner ? sep_y + 8
                           : PANEL_H - MARGIN - row_b_h - 6 - row_a_h;

  // "Charge device", centred in the gap under the separator.
  //
  // Only when that gap exists: with a banner showing, row_a is pulled up to
  // sep_y + 8 and there is no room at all, so this would overprint the
  // insulin row. The ask was to suppress it during an alarm; suppressing it
  // for ANY banner is the same judgement - an alarm, a dead proxy or a pump
  // banner all leave a screen too crowded to add to - and it is also the only
  // layout that fits. The warning returns the moment the banner clears.
  //
  // getBatteryLevel() reports -1 when it cannot tell, which must not read as
  // a flat battery. Sampled here at draw time rather than carried in the
  // state, so a toggle redraw shows the level now, not at the last fetch.
  // The step was decided before this draw and carried in the state, so a
  // toggle redraw cannot disagree with the scheduled one.
  if (!has_banner && s.battState >= 0) {
    int bx = PANEL_W - MARGIN - (BATT_W + BATT_NUB_W);
    int by = sep_y + ((row_a_y - sep_y) - BATT_H) / 2;
    draw_battery_icon(g, bx, by, s.battState);
  }

  char insulin_txt[16];
  if (s.activeInsulin >= 0) snprintf(insulin_txt, sizeof(insulin_txt), "%.1f U", s.activeInsulin);
  else                      snprintf(insulin_txt, sizeof(insulin_txt), "-- U");
  draw_kv_row(g, row_a_y, "Act. insulin", FONT_LABEL, insulin_txt, FONT_VALUE, COLOR_BLACK);

  // The clock here is the moment THIS REFRESH happened, not the reading's
  // own timestamp: "Updated 4 min ago | 09:55" means the reading was from
  // 9:51 and the panel last redrew at 9:55. A timestamp that stops
  // advancing is how you notice the device died.
  int row_b_y = row_a_y + row_a_h + 6;
  long age_s = reading_age_s(s);
  if (age_s >= 0) {
    struct tm tm_now;
    local_tm(time(nullptr), c.timezone, s.dstDelta, tm_now);
    char delta[24], clock[8], label[40];
    age_txt(age_s, delta, sizeof(delta));
    snprintf(clock, sizeof(clock), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    if (age_s < 60) snprintf(label, sizeof(label), "Updated now");
    else            snprintf(label, sizeof(label), "Updated %s ago", delta);
    // Both halves in the small font: at the larger size the clock is wide
    // enough to squeeze the label past its truncation point, and the label
    // is where the staleness lives.
    draw_kv_row(g, row_b_y, label, FONT_LABEL, clock, FONT_LABEL, COLOR_BLACK);
  } else {
    draw_kv_row(g, row_b_y, "Updated", FONT_LABEL, "No data", FONT_LABEL, COLOR_BLACK);
  }

  // --- Alarm / pump banner: the two bottom lines, in reverse -------------
  if (has_banner) {
    String text(banner_src);
    // Two lines were asked for, and two lines is what the fault strings
    // need: the longest run past 50 characters, which no single line of this
    // panel holds legibly. Prefer the larger font when it fits in two lines
    // of it, and drop to the smaller one otherwise so a long message is
    // shown IN FULL rather than truncated - the tail of these strings is
    // the actionable half.
    int inner_w = PANEL_W - 2 * MARGIN - 4;
    const lgfx::IFont *font = FONT_ALARM_L;
    String lines[4];
    int n = wrap_text(g, text.c_str(), inner_w, font, lines, 4);
    if (n > 2) {
      font = FONT_ALARM_S;
      n = wrap_text(g, text.c_str(), inner_w, font, lines, 4);
    }
    if (n > 2) {
      // Still too long even small (only the very longest faults): truncate
      // the second line rather than silently lose the third.
      String tail = lines[1];
      for (int i = 2; i < n; i++) tail += " " + lines[i];
      lines[1] = truncate_to_width(g, tail, inner_w, font);
      n = 2;
    }

#if DEBUG_LAYOUT
    Serial.printf("banner: %d line(s) at %s\n", n,
                  font == FONT_ALARM_L ? "DejaVu18" : "DejaVu12");
    for (int i = 0; i < n; i++) Serial.printf("  [%d] '%s'\n", i, lines[i].c_str());
#endif

    int line_h = font_height(g, font);
    int bar_h = 4 + 2 * line_h + 2 + 4;
    int bar_y = PANEL_H - bar_h;
    g.fillRect(0, bar_y, PANEL_W, bar_h, COLOR_BLACK);
    int ty = bar_y + 4;
    for (int i = 0; i < n; i++) {
      draw_text(g, lines[i].c_str(), MARGIN + 2, ty, font, COLOR_WHITE, COLOR_BLACK);
      ty += line_h + 2;
    }
  }
}

// ------------------------------------------------------- secondary screens

// Shared chrome for the secondary screens. The main screen has no header on
// purpose: there every pixel of the top band belongs to the glucose figure,
// and a caregiver glancing over should recognise that view instantly rather
// than read a label to find out which screen is up.
static int draw_screen_header(LovyanGFX &g, const char *title) {
  draw_text_bold(g, title, MARGIN, 4, FONT_LABEL, COLOR_BLACK);
  int y = 4 + font_height(g, FONT_LABEL) + 3;
  g.drawLine(MARGIN, y, PANEL_W - MARGIN, y, COLOR_BLACK);
  return y + 7;
}

static String fmt_runtime(time_t start) {
  // Hours, because the question this answers is "how many hours does a
  // charge last" - days would round away exactly the resolution wanted.
  if (!start) return "--";
  long secs = (long)(time(nullptr) - start);
  if (secs < 0) return "--";
  float hours = secs / 3600.0f;
  char buf[16];
  if (hours < 100) snprintf(buf, sizeof(buf), "%.1f h", hours);
  else             snprintf(buf, sizeof(buf), "%d h", (int)hours);
  return String(buf);
}

static void pct_txt(int v, char *buf, size_t n) {
  if (v < 0) snprintf(buf, n, "--");
  else       snprintf(buf, n, "%d%%", v);
}

void draw_stats_screen(LovyanGFX &g, const State &s) {
  g.fillScreen(COLOR_WHITE);
  int y = draw_screen_header(g, "GLUCOSE, LAST 24 H");

  // A stacked bar makes the split readable without reading any numbers,
  // which is the whole point of a summary screen. With no colour to work
  // with the segments are distinguished by fill: below-target solid black,
  // in-target hatched, above-target open. That ordering is deliberate - low
  // glucose is the dangerous end, so it gets the heaviest ink and is
  // impossible to miss in peripheral vision.
  int bar_h = 20;
  int bar_w = PANEL_W - 2 * MARGIN;
  int below = s.belowHypo, inrange = s.timeInRange, above = s.aboveHyper;
  if (below >= 0 && inrange >= 0 && above >= 0 && (below + inrange + above) > 0) {
    int total = below + inrange + above;
    int x = MARGIN;
    // The last segment takes the rounding remainder so the three always
    // exactly fill the bar; computing each independently leaves a 1-2px gap
    // that reads as a rendering fault.
    int w_below = bar_w * below / total;
    int w_in    = bar_w * inrange / total;
    int w_above = bar_w - w_below - w_in;
    g.fillRect(x, y, w_below, bar_h, COLOR_BLACK);
    x += w_below;
    for (int hx = x; hx < x + w_in; hx += 3) {
      g.drawLine(hx, y, hx, y + bar_h - 1, COLOR_BLACK);
    }
    x += w_in;
    g.fillRect(x, y, w_above, bar_h, COLOR_WHITE);
    g.drawRect(MARGIN, y, bar_w, bar_h, COLOR_BLACK);
  } else {
    g.drawRect(MARGIN, y, bar_w, bar_h, COLOR_BLACK);
  }
  y += bar_h + 8;

  char v[12], label[24];
  pct_txt(inrange, v, sizeof(v));
  y += draw_kv_row(g, y, "In target", FONT_LABEL, v, FONT_VALUE, COLOR_BLACK) + 4;
  pct_txt(above, v, sizeof(v));
  snprintf(label, sizeof(label), "Above %d", HYPER_THRESHOLD_MGDL);
  y += draw_kv_row(g, y, label, FONT_LABEL, v, FONT_VALUE, COLOR_BLACK) + 4;
  pct_txt(below, v, sizeof(v));
  snprintf(label, sizeof(label), "Below %d", HYPO_THRESHOLD_MGDL);
  y += draw_kv_row(g, y, label, FONT_LABEL, v, FONT_VALUE, COLOR_BLACK) + 4;

  char avg[16];
  if (s.averageSG < 0) snprintf(avg, sizeof(avg), "-- mg/dL");
  else                 snprintf(avg, sizeof(avg), "%d mg/dL", s.averageSG);
  y += 2;
  draw_kv_row(g, y, "Average", FONT_LABEL, avg, FONT_UNIT, COLOR_BLACK);
}

// The last 3 hours as a line, the way the CareLink app shows it, cut down to
// what a 200 px black-and-white panel carries: no colour bands, so the two
// thresholds this display already uses (70 and 180) are dashed lines, and
// the zones outside them are dotted, leaving the in-range band clear.
//
// The window ends NOW, not at the newest reading, so a sensor that stopped
// an hour ago shows as a line that stops an hour short of the right edge.
// Both ends are local wall clock: the readings arrive that way from CareLink
// and "now" is shifted the same way the alarm time is compared.
//
// The scale follows the readings but always keeps 70 and 180 in view: from
// the lower of 70 and the lowest reading, to the higher of 180 and the
// highest, each widened by 10 and rounded out to a multiple of 20, within
// the sensor's 40-400. So the narrowest scale is 60-200: a quiet day's
// wobble stays small instead of filling the panel, while a normal day
// gets about twice the height the fixed 40-300 gave it and a peak above
// 300 is no longer flattened. desktop/ and proxy/display/ use the same rule.
static const int TREND_Y_FLOOR = 40;    // the sensor reads nothing below 40
static const int TREND_Y_CEIL  = 400;   // ... or above 400
static const int TREND_Y_PAD   = 10;
static const int TREND_Y_STEP  = 20;
static const int TREND_Y_TOP_PLAIN = 200;   // top of the narrowest scale: not labelled
// Hour labels: every hour over 3 hours; every third hour over 12, where an
// hour is about 14 px and "09 10 11" would run together. Ticks every hour.
static const int TREND_LABEL_EVERY_12H = 3;

// The scale for readings between lo and hi (0 = no readings).
static void trend_scale(int lo, int hi, int &ymin, int &ymax) {
  int a = HYPO_THRESHOLD_MGDL, b = HYPER_THRESHOLD_MGDL;
  if (lo > 0 && lo < a) a = lo;
  if (hi > b) b = hi;
  a = (a - TREND_Y_PAD) / TREND_Y_STEP * TREND_Y_STEP;
  b = (b + TREND_Y_PAD + TREND_Y_STEP - 1) / TREND_Y_STEP * TREND_Y_STEP;
  ymin = a < TREND_Y_FLOOR ? TREND_Y_FLOOR : a;
  ymax = b > TREND_Y_CEIL ? TREND_Y_CEIL : b;
}

// long12: the 12-hour view, toggled by pressing the switch in on this
// screen; a session always arrives at the 3-hour view. Same rules for both:
// the window ends now, and the scale is from the readings in the window.
void draw_trend_screen(LovyanGFX &g, const State &s, const Config &c,
                       bool long12) {
  g.fillScreen(COLOR_WHITE);
  const int hours = long12 ? 12 : 3;
  const long TREND_WINDOW_S = (long)hours * 3600;
  int top = draw_screen_header(g, long12 ? "LAST 12 HOURS" : "LAST 3 HOURS");
  // Header's right side: the unit, so the scale's bare numbers mean something.
  int uw = text_width(g, "mg/dL", FONT_LABEL);
  draw_text_bold(g, "mg/dL", PANEL_W - MARGIN - uw - 1, 4, FONT_LABEL, COLOR_BLACK);

  const int label_h = font_height(g, FONT_LABEL);
  const int label_w = text_width(g, "180", FONT_LABEL);
  const int x0 = MARGIN;
  const int x1 = PANEL_W - MARGIN - label_w - 4;      // plot's right edge
  const int y0 = top + 2;                              // plot's top
  const int y1 = PANEL_H - label_h - 6;                // plot's baseline

  // The window, in local wall clock. Without a valid clock the newest reading
  // stands in for now: better a correct line with an unknown age than none.
  time_t now = time(nullptr);
  time_t wend = now > 1600000000
                  ? now + (time_t)(c.timezone + s.dstDelta) * 3600
                  : s.trendEnd;
  time_t wstart = wend - TREND_WINDOW_S;
  auto slot_time = [&](int i) {
    return s.trendEnd - (time_t)(TREND_SLOTS_12H - 1 - i) * TREND_STEP_S;
  };
  // An older proxy sends 3 hours only: say so rather than draw them as if
  // the 9 hours before had no readings.
  const bool have = s.trendEnd && (s.trend12h || !long12);

  // The scale, from the readings actually in the window.
  int lo = 0, hi = 0;
  for (int i = 0; i < TREND_SLOTS_12H && have; i++) {
    int v = s.trendSg[i];
    time_t t = slot_time(i);
    if (!v || t < wstart || t > wend) continue;
    if (!lo || v < lo) lo = v;
    if (v > hi) hi = v;
  }
  int ymin, ymax;
  trend_scale(lo, hi, ymin, ymax);
  auto ymap = [&](int v) {
    if (v < ymin) v = ymin;
    if (v > ymax) v = ymax;
    return y1 - (v - ymin) * (y1 - y0) / (ymax - ymin);
  };

  // Outside the target range, dotted: below 70 and above 180 alike, so the
  // clear band in between is "in range" at a glance. Which side a dotted
  // stretch is on, the 70 and 180 labels say.
  auto dots = [&](int ya, int yb) {
    for (int y = ya; y < yb; y += 3)
      for (int x = x0 + ((y / 3) % 2) * 2; x <= x1; x += 4)
        g.drawPixel(x, y, COLOR_BLACK);
  };
  dots(ymap(HYPO_THRESHOLD_MGDL) + 3, y1);
  dots(y0 + 1, ymap(HYPER_THRESHOLD_MGDL) - 1);
  // The two thresholds, dashed, labelled on the right.
  for (int v : {HYPO_THRESHOLD_MGDL, HYPER_THRESHOLD_MGDL}) {
    int y = ymap(v);
    for (int x = x0; x <= x1; x += 6) g.drawFastHLine(x, y, 3, COLOR_BLACK);
    char lab[8];
    snprintf(lab, sizeof(lab), "%d", v);
    draw_text_bold(g, lab, x1 + 4, y - label_h / 2, FONT_LABEL, COLOR_BLACK);
  }
  // The target, always: a finer dotted line than the thresholds' dashes, so
  // it reads as a reference rather than a limit. Its label is left out when
  // the scale is so wide that it would run into the 70's.
  {
    int y = ymap(TARGET_MGDL);
    for (int x = x0; x <= x1; x += 3) g.drawPixel(x, y, COLOR_BLACK);
    if (ymap(HYPO_THRESHOLD_MGDL) - y >= label_h) {
      char lab[8];
      snprintf(lab, sizeof(lab), "%d", TARGET_MGDL);
      draw_text_bold(g, lab, x1 + 4, y - label_h / 2, FONT_LABEL, COLOR_BLACK);
    }
  }
  // The top of the scale, labelled only above the narrowest scale's 200:
  // there it would sit right on top of 180 and say nothing, while above it
  // it is what a high peak is read against.
  if (ymax > TREND_Y_TOP_PLAIN && ymap(HYPER_THRESHOLD_MGDL) - y0 >= label_h + 2) {
    char lab[8];
    snprintf(lab, sizeof(lab), "%d", ymax);
    draw_text_bold(g, lab, x1 + 4, y0, FONT_LABEL, COLOR_BLACK);
    for (int x = x0; x <= x1; x += 12) g.drawPixel(x, y0, COLOR_BLACK);
  }
  g.drawFastHLine(x0, y1, x1 - x0 + 1, COLOR_BLACK);
  auto xmap = [&](time_t t) {
    return x0 + (int)((long)(t - wstart) * (x1 - x0) / TREND_WINDOW_S);
  };

  // Hour ticks and labels along the baseline.
  if (wend) {
    for (time_t h = (wstart / 3600 + 1) * 3600; h <= wend; h += 3600) {
      int x = xmap(h);
      g.drawFastVLine(x, y1, 4, COLOR_BLACK);
      struct tm tm;
      gmtime_r(&h, &tm);
      if (long12 && tm.tm_hour % TREND_LABEL_EVERY_12H) continue;
      // Hours only: "08:00 09:00 10:00" ran together on the panel.
      char lab[8];
      snprintf(lab, sizeof(lab), "%02d", tm.tm_hour);
      int w = text_width(g, lab, FONT_LABEL);
      int lx = x - w / 2;
      if (lx < 0) lx = 0;
      if (lx + w > x1) lx = x1 - w;
      draw_text_bold(g, lab, lx, y1 + 5, FONT_LABEL, COLOR_BLACK);
    }
  }

  // The line: three pixels thick, broken wherever a slot has no reading.
  int drawn = 0, px = -1, py = -1;
  for (int i = 0; i < TREND_SLOTS_12H && have; i++) {
    time_t t = slot_time(i);
    int v = s.trendSg[i];
    if (!v || t < wstart || t > wend) { px = -1; continue; }
    int x = xmap(t), y = ymap(v);
    if (px >= 0) {
      for (int d = -1; d <= 1; d++) g.drawLine(px, py + d, x, y + d, COLOR_BLACK);
    } else {
      g.fillRect(x - 1, y - 1, 3, 3, COLOR_BLACK);
    }
    px = x; py = y;
    drawn++;
  }
  // The newest reading as a dot with a white ring, as in the app.
  if (px >= 0) {
    g.fillCircle(px, py, 5, COLOR_WHITE);
    g.fillCircle(px, py, 4, COLOR_BLACK);
  }

  if (!drawn) {
    const char *msg = !s.trendEnd ? "No trend from proxy"
                    : !have       ? "Proxy sends 3 h only"
                    : long12      ? "No readings in 12 h"
                                  : "No readings in 3 h";
    int w = text_width(g, msg, FONT_LABEL);
    int mx = x0 + (x1 - x0 - w) / 2;
    int my = (y0 + y1) / 2 - label_h / 2;
    g.fillRect(mx - 3, my - 2, w + 6, label_h + 4, COLOR_WHITE);
    draw_text(g, msg, mx, my, FONT_LABEL, COLOR_BLACK);
  }
}

void draw_pump_screen(LovyanGFX &g, const State &s, const Config &c) {
  // Between glucose and the technical screen: what a caregiver plans around
  // rather than reacts to. None of it is urgent enough for the main screen,
  // all of it is what you want to know before leaving the house.
  g.fillScreen(COLOR_WHITE);
  int y = draw_screen_header(g, "PUMP & SENSOR");

  const char *patient = c.patient.length() ? c.patient.c_str()
                      : (s.patient[0] ? s.patient : "--");

  char insulin[24];
  if (s.reservoirUnits < 0)   snprintf(insulin, sizeof(insulin), "--");
  else if (s.reservoirPct < 0) snprintf(insulin, sizeof(insulin), "%d U", (int)lroundf(s.reservoirUnits));
  else snprintf(insulin, sizeof(insulin), "%d U  %d%%",
                (int)lroundf(s.reservoirUnits), s.reservoirPct);

  // 255 is the pump's "no sensor / not settled yet" sentinel, not a life of
  // ten and a half days.
  char sensor[16];
  if (s.sageHours >= 255 || s.sageHours < 0) snprintf(sensor, sizeof(sensor), "--");
  else if (s.sageHours >= 48) snprintf(sensor, sizeof(sensor), "%dd %dh",
                                       s.sageHours / 24, s.sageHours % 24);
  else snprintf(sensor, sizeof(sensor), "%d h", s.sageHours);

  char batt[8];
  pct_txt(s.batteryPct, batt, sizeof(batt));

  // Values in the larger font: four rows where the info screen has eight, so
  // the room is there, and these are numbers read across a room rather than
  // settings leaned in to check.
  struct { const char *label; const char *value; } rows[] = {
    {"Patient",   patient},
    {"Insulin",   insulin},
    {"Sensor",    sensor},
    {"Pump batt", batt},
  };
  for (auto &r : rows) {
    y += draw_kv_row(g, y, r.label, FONT_LABEL, r.value, FONT_VALUE, COLOR_BLACK) + 6;
  }
}

// How one poll went, in a word or two: the info screen's "Last poll" row and
// the history page. NO_SENSOR counts as ok - the link worked.
static const char *poll_result_text(int link) {
  switch (link) {
    case LINK_OK:
    case LINK_NO_SENSOR:     return "ok";
    case LINK_PROXY_NO_DATA: return "proxy empty";
    case LINK_PROXY_LOGIN:   return "login";
    case LINK_NO_PROXY:      return "no proxy";
    case LINK_NO_WIFI:       return "no WiFi";
    case LINK_ABORTED:       return "cut off";
    default:                 return "?";
  }
}

void draw_info_screen(LovyanGFX &g, const State &s, const Config &c,
                      time_t session_start) {
  g.fillScreen(COLOR_WHITE);
  int y = draw_screen_header(g, "DEVICE & NETWORK");

  char batt[8];
  int level = M5.Power.getBatteryLevel();
  if (level < 0) snprintf(batt, sizeof(batt), "--");
  else           snprintf(batt, sizeof(batt), "%d%%", level);

  // How the last poll went, and when: what someone sent here by "No WiFi"
  // or "Update failed" on the main screen came to find out. It took the
  // place of a plain clock row, which the main screen's "Updated" row
  // already carries. Reads the poll's own result, before the three-poll
  // smoothing, so a fault shows here from the first poll that has it.
  char last_poll[24];
  if (s.pollAt > 1600000000 && time(nullptr) > 1600000000) {
    const char *how = poll_result_text(s.rawLink);
    struct tm at;
    local_tm(s.pollAt, c.timezone, s.dstDelta, at);
    snprintf(last_poll, sizeof(last_poll), "%02d:%02d %s", at.tm_hour, at.tm_min, how);
  } else {
    snprintf(last_poll, sizeof(last_poll), "--");
  }

  // Signal at the last poll, with a word for it: the number alone means
  // nothing to most people, and this row exists so a board can be placed by
  // walking around with it. Measured on these boards: -74 dBm gave the odd
  // slow fetch, -79 and -82 regular ones and lookups that needed retries.
  char signal[24];
  if (s.rssi == 0)        snprintf(signal, sizeof(signal), "--");
  else if (s.rssi >= -67) snprintf(signal, sizeof(signal), "%d dBm good", s.rssi);
  else if (s.rssi >= -75) snprintf(signal, sizeof(signal), "%d dBm fair", s.rssi);
  else                    snprintf(signal, sizeof(signal), "%d dBm weak", s.rssi);

  char port[8], version[16];
  snprintf(port, sizeof(port), "%u", c.proxyport);
  snprintf(version, sizeof(version), "V%s", VERSION_STR);
  String runtime = fmt_runtime(session_start);

  // Battery first: the only line that changes on its own and the only one
  // that predicts the device silently dying. NTP server and timezone used to
  // sit here and were dropped rather than squeezed - both are write-once
  // settings readable from the config page, where runtime and battery change
  // by themselves and are what someone comes to this screen to find.
  struct { const char *label; const char *value; } rows[] = {
    {"Battery",  batt},
    {"Runtime",  runtime.c_str()},
    {"Last poll", last_poll},
    // The network the last poll joined, not merely the one configured - with
    // several slots, "which am I on?" is the useful answer. Written on every
    // poll, failed ones included; "none" when the last one joined nothing.
    // (Until 0.9 only a poll that brought data updated it, so a board that
    // had moved on from a hotspot that was switched off went on naming it.)
    {"WiFi",     s.ssid[0] ? s.ssid
                 : s.rawLink == LINK_NO_WIFI ? "none"
                 : (c.wifi[0].ssid.length() ? c.wifi[0].ssid.c_str() : "--")},
    {"Signal",   signal},
    {"IP",       s.ip[0] ? s.ip : "--"},
    // Port on its own row, not as host:port, so a long host name keeps the
    // whole width.
    {"Proxy",    c.proxyaddr.length() ? c.proxyaddr.c_str() : "--"},
    {"Port",     port},
    {"Version",  version},
  };
  int lh = font_height(g, FONT_LABEL);
  for (auto &r : rows) {
    // An SSID, an IPv4 address, a hostname: the one place on any screen
    // where user-supplied text of unbounded length is rendered, so these are
    // right-aligned and truncated rather than trusted to fit.
    draw_kv_row(g, y, r.label, FONT_LABEL, r.value, FONT_LABEL, COLOR_BLACK);
    y += lh + 5;
  }
}

// What a person sends to whoever looks after the set: counts since the last
// reset, so "Watchdog 2, last 03:14" can be read out over the phone. Same
// row layout as Device & Network, and the same number of rows, which is
// what that screen proved fits.
static void draw_debug_screen(LovyanGFX &g, const State &s, const Config &c,
                              const DebugStats *d) {
  g.fillScreen(COLOR_WHITE);
  int y = draw_screen_header(g, "DEBUG SINCE RESET");

  char polls[24] = "--", wdt[24] = "--", resets[16] = "--", awake[24] = "--";
  char mv[12] = "--", mac[20] = "--";
  if (d) {
    snprintf(polls, sizeof(polls), "%lu/%lu ok",
             (unsigned long)d->polls, (unsigned long)d->pollsOk);
    if (d->watchdog && d->watchdogAt > 1600000000) {
      struct tm at;
      local_tm(d->watchdogAt, c.timezone, s.dstDelta, at);
      snprintf(wdt, sizeof(wdt), "%u, %02d:%02d", (unsigned)d->watchdog,
               at.tm_hour, at.tm_min);
    } else {
      snprintf(wdt, sizeof(wdt), "%u", (unsigned)d->watchdog);
    }
    snprintf(resets, sizeof(resets), "%u/%u", (unsigned)d->brownout, (unsigned)d->crashes);
    // Average / longest of the polls that finished: the battery cost, and
    // how close the slowest came to the watchdog's 3 minutes.
    if (d->awakeN)
      snprintf(awake, sizeof(awake), "%.1f/%.0f s",
               d->awakeSumMs / 1000.0 / d->awakeN, d->awakeMaxMs / 1000.0);
  }
  // Measured now, with the radio down: on USB it reads the charging voltage.
  int bmv = battery_mv();
  if (bmv > 0) snprintf(mv, sizeof(mv), "%d", bmv);
  // The WiFi station MAC, from eFuse - readable with the radio off. What a
  // router's client list or a MAC filter knows the board by.
  uint8_t m[6];
  if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK)
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);

  // The configured networks first: which WiFi this board can join at all
  // (slot 2 is the Pi's hotspot, usually learned from the proxy). SSIDs are
  // user text of any length, so they are cut to fit beside their label.
  static const char *slot_label[WIFI_SLOTS] = {"WiFi 1", "WiFi 2", "Fallback"};
  String ssid[WIFI_SLOTS];
  for (int i = 0; i < WIFI_SLOTS; i++) {
    int room = PANEL_W - 2 * MARGIN - text_width(g, slot_label[i], FONT_LABEL) - 8;
    ssid[i] = c.wifi[i].ssid.length() ? fit_text(g, c.wifi[i].ssid, FONT_LABEL, room)
                                      : String("--");
  }

  struct { const char *label; const char *value; } rows[] = {
    {slot_label[0], ssid[0].c_str()},
    {slot_label[1], ssid[1].c_str()},
    {slot_label[2], ssid[2].c_str()},
    {"MAC",         mac},
    {"Polls",       polls},
    {"Watchdog",    wdt},
    {"Brownout/crash", resets},
    {"Awake",       awake},
    {"Batt mV",     mv},
  };
  int lh = font_height(g, FONT_LABEL);
  for (auto &r : rows) {
    draw_kv_row(g, y, r.label, FONT_LABEL, r.value, FONT_LABEL, COLOR_BLACK);
    y += lh + 5;
  }
}

// The debug screen's second page: the last polls, newest first, one row
// each - time and network on the left (the network cut short when it does
// not fit), the result on the right. Answers "did it change networks, and
// did that work?" without a laptop.
static void draw_history_screen(LovyanGFX &g, const State &s, const Config &c,
                                const DebugStats *d) {
  g.fillScreen(COLOR_WHITE);
  int y = draw_screen_header(g, "LAST POLLS");
  int lh = font_height(g, FONT_LABEL);
  int shown = 0;
  for (int i = 1; d && i <= POLL_HISTORY; i++) {
    const DebugStats::PollRec &r = d->hist[(d->histNext + POLL_HISTORY - i) % POLL_HISTORY];
    if (!r.at) break;
    char when[8] = "--:--";
    if (r.at > 1600000000) {
      struct tm at;
      local_tm(r.at, c.timezone, s.dstDelta, at);
      snprintf(when, sizeof(when), "%02d:%02d", at.tm_hour, at.tm_min);
    }
    const char *net = (r.slot >= 0 && r.slot < WIFI_SLOTS && c.wifi[r.slot].ssid.length())
                        ? c.wifi[r.slot].ssid.c_str() : "-";
    String left = String(when) + " " + net;
    draw_kv_row(g, y, left.c_str(), FONT_LABEL, poll_result_text(r.link), FONT_LABEL,
                COLOR_BLACK);
    y += lh + 5;
    shown++;
  }
  if (!shown) draw_text(g, "No polls yet", MARGIN, y, FONT_LABEL, COLOR_BLACK);
}

// After a brownout: the battery could not carry the WiFi radio. This is
// likely the last image the board draws before it runs flat, and e-paper
// keeps it with no power, so it has to make sense on its own for days: an
// empty battery, a plug, what to do, and the time of the last reading. No
// glucose value - an old number left up for hours must not pass for a
// current one.
static void draw_charge_screen(LovyanGFX &g, const State &s, const Config &c) {
  g.fillScreen(COLOR_WHITE);

  // Empty battery, 3 px outline, nub on the right.
  const int bx = 38, by = 22, bw = 112, bh = 54;
  for (int i = 0; i < 3; i++)
    g.drawRect(bx + i, by + i, bw - 2 * i, bh - 2 * i, COLOR_BLACK);
  g.fillRect(bx + bw, by + (bh - 22) / 2, 9, 22, COLOR_BLACK);

  // A plug pointing up at it, with its cable.
  const int px = 100 - 13, py = 92;
  g.fillRect(px + 5, py - 9, 5, 10, COLOR_BLACK);    // prongs
  g.fillRect(px + 16, py - 9, 5, 10, COLOR_BLACK);
  g.fillRoundRect(px, py, 26, 20, 3, COLOR_BLACK);    // body
  g.fillRect(100 - 2, py + 20, 5, 12, COLOR_BLACK);   // cable

  const char *msg = "Please charge";
  int mw = text_width(g, msg, FONT_VALUE);
  draw_text(g, msg, (PANEL_W - mw) / 2, 134, FONT_VALUE, COLOR_BLACK);

  if (s.lastGoodAt > 1600000000) {
    struct tm at;
    local_tm(s.lastGoodAt, c.timezone, s.dstDelta, at);
    char line[32];
    snprintf(line, sizeof(line), "Last reading %02d:%02d", at.tm_hour, at.tm_min);
    int lw = text_width(g, line, FONT_LABEL);
    draw_text(g, line, (PANEL_W - lw) / 2, 172, FONT_LABEL, COLOR_BLACK);
  }
}

void draw_current_screen(LovyanGFX &g, int screen, const State &s,
                         const Config &c, time_t session_start, bool trend12h,
                         const DebugStats *dbg) {
  switch (screen) {
    case SCREEN_CHARGE: draw_charge_screen(g, s, c);              break;
    case SCREEN_DEBUG: draw_debug_screen(g, s, c, dbg);            break;
    case SCREEN_HISTORY: draw_history_screen(g, s, c, dbg);        break;
    case SCREEN_TREND: draw_trend_screen(g, s, c, trend12h);       break;
    case SCREEN_STATS: draw_stats_screen(g, s);                    break;
    case SCREEN_PUMP:  draw_pump_screen(g, s, c);                  break;
    case SCREEN_INFO:  draw_info_screen(g, s, c, session_start);   break;
    default:           draw_main_screen(g, s, c);                  break;
  }
}

void draw_status_screen(LovyanGFX &g, const char *msg) {
  g.fillScreen(COLOR_WHITE);
  int y = MARGIN;
  int lh = font_height(g, FONT_LABEL);
  String rest(msg);
  while (rest.length()) {
    int nl = rest.indexOf('\n');
    String line = (nl < 0) ? rest : rest.substring(0, nl);
    rest = (nl < 0) ? String() : rest.substring(nl + 1);
    String wrapped[6];
    int n = wrap_text(g, line.c_str(), PANEL_W - 2 * MARGIN, FONT_LABEL, wrapped, 6);
    for (int i = 0; i < n; i++) {
      draw_text(g, wrapped[i].c_str(), MARGIN, y, FONT_LABEL, COLOR_BLACK);
      y += lh + 2;
    }
  }
}

void draw_wipe_dialog(LovyanGFX &g, bool yes_selected) {
  g.fillScreen(COLOR_WHITE);
  int y = draw_screen_header(g, "WIPE CONFIGURATION");

  int lh = font_height(g, FONT_LABEL);
  String lines[4];
  int n = wrap_text(g, "Erase all settings and restart in setup mode?",
                    PANEL_W - 2 * MARGIN, FONT_LABEL, lines, 4);
  for (int i = 0; i < n; i++) {
    draw_text(g, lines[i].c_str(), MARGIN, y, FONT_LABEL, COLOR_BLACK);
    y += lh + 2;
  }
  y += 6;

  // The selection is shown by inversion, the only emphasis a monochrome
  // panel has that cannot be mistaken for a border style.
  const char *opts[2] = {"No", "Yes"};
  int vh = font_height(g, FONT_VALUE);
  int box_h = vh + 8;
  int box_w = 100;
  int box_x = (PANEL_W - box_w) / 2;
  for (int i = 0; i < 2; i++) {
    bool sel = (i == 1) == yes_selected;
    uint32_t fg = sel ? COLOR_WHITE : COLOR_BLACK;
    uint32_t bg = sel ? COLOR_BLACK : COLOR_WHITE;
    if (sel) g.fillRect(box_x, y, box_w, box_h, COLOR_BLACK);
    else     g.drawRect(box_x, y, box_w, box_h, COLOR_BLACK);
    int tw = text_width(g, opts[i], FONT_VALUE);
    draw_text(g, opts[i], box_x + (box_w - tw) / 2, y + 4, FONT_VALUE, fg, bg);
    y += box_h + 6;
  }

  const char *hint = "Flick to choose, press";
  int hw = text_width(g, hint, FONT_LABEL);
  draw_text(g, hint, (PANEL_W - hw) / 2, PANEL_H - MARGIN - lh, FONT_LABEL, COLOR_BLACK);
}

void draw_join_screen(LovyanGFX &g, const char *ssid, const char *pass) {
  g.fillScreen(COLOR_WHITE);
  // The standard WiFi QR payload, read natively by Android and iOS cameras.
  // Neither the SSID nor the generated password contains a character that
  // this format would need escaped (; , : \ ").
  String qr = String("WIFI:T:WPA;S:") + ssid + ";P:" + pass + ";;";
  // 116px: version 3 is 29 modules, so 4px each, which an e-paper panel
  // renders crisply. The panel's white surround serves as the quiet zone.
  const int qr_w = 116;
  g.qrcode(qr.c_str(), (PANEL_W - qr_w) / 2, 6, qr_w, 3);

  int y = 6 + qr_w + 6;
  const char *lines[] = {"Scan to join, or connect to", ssid};
  int lh = font_height(g, FONT_LABEL);
  for (const char *l : lines) {
    int w = text_width(g, l, FONT_LABEL);
    draw_text(g, l, (PANEL_W - w) / 2, y, FONT_LABEL, COLOR_BLACK);
    y += lh + 1;
  }
  int pw = text_width(g, pass, FONT_UNIT);
  draw_text(g, pass, (PANEL_W - pw) / 2, y + 1, FONT_UNIT, COLOR_BLACK);
#if DEBUG_LAYOUT
  Serial.printf("join: qr ends y=%d, password at y=%d w=%d h=%d\n",
                6 + qr_w, y + 1, pw, font_height(g, FONT_UNIT));
#endif
}
