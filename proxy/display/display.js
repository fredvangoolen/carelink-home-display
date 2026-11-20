// The live display page served by the proxy at /display.
//
// Same rules as the Core Ink firmware and desktop/minimed_desktop.py: reading
// age from epochs, a stale banner after 20 minutes, link faults named rather
// than shown as "---", alarms only while recent and still current. Change
// them in all three places or none.
//
// SECURITY: every value from CareLink is inserted with textContent, never as
// HTML. Script on this origin could post a token to the proxy; see
// DISPLAY_HEADERS in carelink_client2_proxy.py.
//
// Copyright 2026, Frederik. AGPL-3.0, like the rest of proxy/.
"use strict";

const POLL_S = 60;
const FETCH_TIMEOUT_MS = 10000;
const FAIL_STREAK_SHOWN = 3;
const STALE_READING_S = 20 * 60;
const ALARM_RECENCY_S = 15 * 60;
const ALARM_REPEAT_S = 5 * 60;
const HYPO_MGDL = 70;
const HYPER_MGDL = 180;
const TARGET_MGDL = 100;   // the pump's glucose target: a reference line on the trend, no rule uses it
const NEED_LOGIN_MARKER = "Valid token required";
// The trend: /carelink/trend, 36 readings 5 minutes apart. The display's
// window and scale: it ends NOW, so a line that stops short means no
// readings since; the scale follows the readings but always keeps 70 and
// 180 in view (trendScale).
const TREND_SLOTS = 36;
const TREND_STEP_S = 300;
const TREND_Y_FLOOR = 40, TREND_Y_CEIL = 400;   // what the sensor can read
const TREND_Y_PAD = 10, TREND_Y_STEP = 20;
const TREND_Y_TOP_PLAIN = 200;   // the narrowest scale's top: not labelled, it would sit on 180

// [ymin, ymax] for readings lo..hi (0 = none), the display's rule: the lower
// of 70 and lo to the higher of 180 and hi, widened by 10, rounded out to 20,
// within 40-400. At least 60-200, so a quiet line stays quiet.
function trendScale(lo, hi) {
  const a = lo > 0 ? Math.min(HYPO_MGDL, lo) : HYPO_MGDL;
  const b = Math.max(HYPER_MGDL, hi);
  return [Math.max(TREND_Y_FLOOR, Math.floor((a - TREND_Y_PAD) / TREND_Y_STEP) * TREND_Y_STEP),
          Math.min(TREND_Y_CEIL, Math.ceil((b + TREND_Y_PAD) / TREND_Y_STEP) * TREND_Y_STEP)];
}

const LINK = { OK: 0, NO_SENSOR: 1, PROXY_NO_DATA: 2, PROXY_LOGIN: 3, NO_PROXY: 4 };
const LINK_TEXT = ["", "No sensor data", "Proxy has no data", "CareLink login needed", "No link to proxy"];
const LINK_NAME = ["ok", "no sensor data", "proxy has no data", "proxy needs login", "no proxy"];
const ARROWS = {
  UP_TRIPLE: "↑↑↑", UP_DOUBLE: "↑↑", UP: "↑", NONE: "→",
  DOWN: "↓", DOWN_DOUBLE: "↓↓", DOWN_TRIPLE: "↓↓↓",
};

const $ = (id) => document.getElementById(id);

const model = {
  state: null, link: LINK.OK, failStreak: 0, skew: 0, lastGoodSg: 0,
  lastPoll: null, answer: "-", raw: "", trend: null,
};
let audio = null, alarmKey = null, alarmSounded = 0, pollTimer = null, polling = false, rawShown = null;

// ------------------------------------------------------------ helpers

const now = () => Date.now() / 1000 + model.skew;   // the proxy's clock

function isNum(v) { return typeof v === "number" && isFinite(v); }
function get(o, ...keys) {
  for (const k of keys) {
    if (o === null || typeof o !== "object" || !(k in o)) return undefined;
    o = o[k];
  }
  return o === null ? undefined : o;
}

function ageText(s) {
  const m = Math.floor(s / 60);
  if (m < 1) return "now";
  if (m < 60) return m + " min";
  if (m < 1440) return Math.floor(m / 60) + " h " + (m % 60) + " min";
  if (m < 2880) return "1 day";
  return Math.floor(m / 1440) + " days";
}

function hhmm(epochS) {
  const d = new Date(epochS * 1000);
  return String(d.getHours()).padStart(2, "0") + ":" + String(d.getMinutes()).padStart(2, "0");
}

function faultText(id) {
  const canon = FAULT_MAP[id] || id;
  return FAULT_TEXT[canon] || "Unknown error code " + id;
}

// lastAlarm.dateTime's digits are the pump's LOCAL wall-clock time, whatever
// its suffix says, so read them as this browser's local time. Right while the
// browser is in the patient's time zone. 0 on failure: never a recent alarm.
function parseLocal(dt) {
  const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})/.exec(dt || "");
  if (!m) return 0;
  const d = new Date(+m[1], m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  return isNaN(d) ? 0 : d.getTime() / 1000;
}

function resolveAlarm(a, sg, t) {
  if (!a || typeof a !== "object") return null;
  const at = parseLocal(a.dateTime);
  if (!at || at <= t - ALARM_RECENCY_S) return null;
  const id = String(a.faultId || "");
  if (!id) return null;
  const canon = FAULT_MAP[id] || id;
  // A low/high notification is stale the moment the reading recovers.
  if (sg > 0) {
    if (LOW_GLUCOSE_IDS.includes(canon) && sg > HYPO_MGDL) return null;
    if (HIGH_GLUCOSE_IDS.includes(canon) && sg < HYPER_MGDL) return null;
  }
  return { text: faultText(id), at, isAlarm: a.type === "ALARM", key: a.GUID || id + "@" + a.dateTime };
}

function parse(doc) {
  const have = get(doc, "conduitInRange") === true && get(doc, "conduitMedicalDeviceInRange") === true;
  const sg = get(doc, "lastSG", "sg");
  const ms = get(doc, "lastConduitUpdateServerDateTime");
  const s = {
    haveData: have,
    sg: isNum(sg) && sg > 0 ? Math.round(sg) : 0,
    lastUpdate: isNum(ms) && ms > 0 ? Math.floor(ms / 1000) : 0,
    trend: "NONE", insulin: null, battery: null, reservoir: null, sageHours: 255, sensorState: "",
    lastAlarm: get(doc, "lastAlarm"),
    pumpInRange: get(doc, "conduitMedicalDeviceInRange"),
    phoneInRange: get(doc, "conduitInRange"),
    stats: ["timeInRange", "aboveHyperLimit", "belowHypoLimit", "averageSG"].map((k) => {
      const v = get(doc, k); return isNum(v) ? v : -1;
    }),
  };
  if (have) {
    const num = (v) => (isNum(v) ? v : null);
    s.insulin = num(get(doc, "activeInsulin", "amount"));
    s.battery = num(get(doc, "pumpBatteryLevelPercent"));
    s.reservoir = num(get(doc, "reservoirRemainingUnits"));
    s.sageHours = isNum(get(doc, "sensorDurationHours")) ? get(doc, "sensorDurationHours") : 255;
    s.sensorState = String(get(doc, "sensorState") || "");
    if ((get(doc, "therapyAlgorithmState", "autoModeShieldState") || "FEATURE_OFF") !== "FEATURE_OFF")
      s.trend = String(get(doc, "lastSGTrend") || "NONE");
  }
  let banner = "";
  const sys = get(doc, "systemStatusMessage");
  if (typeof sys === "string" && sys && sys !== "NO_ERROR_MESSAGE") banner = sys;
  const pb = get(doc, "pumpBannerState");
  if (Array.isArray(pb) && pb[0] && pb[0].type) banner = String(pb[0].type);
  // Plain words for the one banner that reads like a fault code; the display
  // and the desktop monitor do the same.
  if (banner === "DELIVERY_SUSPEND") banner = "Pump suspended";
  s.banner = banner.replace(/_/g, " ");
  return s;
}

function sensorText(s) {
  if (s.sensorState === "CHANGE_SENSOR") return "change";
  const h = s.sageHours;
  if (!isNum(h) || h === 255) return "";
  return h > 9 ? Math.round(h / 24) + " d" : h + " h";
}

// ------------------------------------------------------------ polling

async function fetchWithTimeout(url) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), FETCH_TIMEOUT_MS);
  try {
    return await fetch(url, { cache: "no-store", signal: ctl.signal });
  } finally {
    clearTimeout(timer);
  }
}

async function proxyWantsLogin() {
  try {
    const r = await fetchWithTimeout("/");
    return r.ok && (await r.text()).includes(NEED_LOGIN_MARKER);
  } catch (e) {
    return false;
  }
}

// /carelink/trend -> {end: epoch, sg: [36]} or null. "end" is local wall
// clock like lastAlarm.dateTime, read the same way (parseLocal).
async function fetchTrend() {
  try {
    const r = await fetchWithTimeout("/carelink/trend");
    if (!r.ok) return null;
    const doc = await r.json();
    const end = parseLocal(doc && doc.end);
    if (!doc || doc.step !== TREND_STEP_S || !Array.isArray(doc.sg) || doc.sg.length !== TREND_SLOTS || !end)
      return null;
    return { end, sg: doc.sg.map((v) => (isNum(v) && v > 0 ? Math.round(v) : 0)) };
  } catch (e) {
    return null;
  }
}

async function poll() {
  if (polling) return;          // a tab returning mid-poll must not start a second chain
  polling = true;
  clearTimeout(pollTimer);
  try {
    await pollOnce();
  } finally {
    polling = false;
    pollTimer = setTimeout(poll, POLL_S * 1000);
  }
}

async function pollOnce() {
  model.lastPoll = Date.now();
  let r, raw;
  try {
    r = await fetchWithTimeout("/carelink/nohistory");
    raw = await r.text();
  } catch (e) {
    r = null;
    model.answer = "no answer: " + (e.name === "AbortError" ? "timed out" : e.message);
  }
  if (r) {
    const date = Date.parse(r.headers.get("Date") || "");
    if (!isNaN(date)) model.skew = date / 1000 - Date.now() / 1000;
    model.answer = "HTTP " + r.status + ", " + raw.length + " bytes";
  }
  if (!r || !r.ok) {
    // One missed poll is a hiccup; the age of the last reading keeps counting.
    model.failStreak++;
    if (model.failStreak >= FAIL_STREAK_SHOWN) model.link = LINK.NO_PROXY;
  } else {
    model.failStreak = 0;
    model.raw = raw;
    let doc = null;
    try { doc = JSON.parse(raw); } catch (e) { doc = null; }
    // A proxy holding nothing answers with the JSON string "". That replaces
    // the old reading: the page may not keep showing one the proxy no longer
    // stands behind.
    if (!doc || typeof doc !== "object" || Array.isArray(doc)) {
      model.state = null;
      model.link = (await proxyWantsLogin()) ? LINK.PROXY_LOGIN : LINK.PROXY_NO_DATA;
    } else {
      model.state = parse(doc);
      model.link = model.state.sg > 0 ? LINK.OK : LINK.NO_SENSOR;
      if (model.state.sg > 0) model.lastGoodSg = model.state.sg;
      // Kept across a failed fetch, like the display: one miss must not
      // blank the chart, and the window's own end keeps it honest.
      const t = await fetchTrend();
      if (t) model.trend = t;
    }
  }
  draw();
}

// ------------------------------------------------------------ view

function age() {
  const s = model.state;
  return s && s.lastUpdate ? Math.max(0, now() - s.lastUpdate) : null;
}

function alarm() {
  if (!model.state || model.link === LINK.NO_PROXY) return null;
  return resolveAlarm(model.state.lastAlarm, model.state.sg, now());
}

function banner() {
  const a = alarm();
  if (a) return [a.text + " (" + hhmm(a.at) + ")", "alarm"];
  const ag = age();
  // With no reading but the pump in touch with the phone, the pump's own
  // banner (DELIVERY SUSPEND when it is disconnected) says why; it outranks
  // "No sensor data", which only restates it.
  const s = model.state;
  if (model.link === LINK.NO_SENSOR && s && s.banner && s.pumpInRange === true) return [s.banner, "pump"];
  if (model.link === LINK.NO_SENSOR && ag !== null) return [LINK_TEXT[model.link] + " (" + ageText(ag) + ")", "link"];
  if (model.link !== LINK.OK) return [LINK_TEXT[model.link], "link"];
  if (ag !== null && ag >= STALE_READING_S) return ["No new reading for " + ageText(ag), "stale"];
  if (model.state && model.state.banner) return [model.state.banner, "pump"];
  return ["", null];
}

function setText(id, text) { $(id).textContent = text; }

function draw() {
  const s = model.state;
  const current = s !== null && model.link !== LINK.NO_PROXY;
  const sg = current ? s.sg : 0;

  const reading = $("reading");
  reading.classList.toggle("low", sg > 0 && sg < HYPO_MGDL);
  reading.classList.toggle("high", sg > HYPER_MGDL);
  setText("sg", sg ? String(sg) : "---");
  setText("trend", sg ? ARROWS[s.trend] || ARROWS.NONE : "");
  setText("prev", !sg && model.lastGoodSg ? "[" + model.lastGoodSg + "]" : "");

  const [text, kind] = banner();
  const b = $("banner");
  b.hidden = !text;
  b.className = kind || "";
  b.textContent = text;
  document.title = (sg ? sg + " " : "") + (text ? "⚠ " : "") + "CareLink display";

  const show = (v, f) => (current && v !== null && v !== undefined ? f(v) : "--");
  setText("insulin", show(s && s.insulin, (v) => v.toFixed(1) + " U"));
  setText("battery", show(s && s.battery, (v) => v + " %"));
  setText("reservoir", show(s && s.reservoir, (v) => v.toFixed(0) + " U"));
  setText("sensor", current ? sensorText(s) || "--" : "--");
  const st = s ? s.stats : [-1];
  setText("stats", current && Math.min(...st) >= 0
    ? "24 h: " + st[0] + "% in range, " + st[1] + "% above, " + st[2] + "% below, average " + st[3] : "");

  const ag = age();
  setText("updated", ag === null ? "Updated: no data" : ag < 60 ? "Updated now" : "Updated " + ageText(ag) + " ago");
  setText("clock", hhmm(now()));

  // Debug panel.
  setText("d-poll", model.lastPoll ? new Date(model.lastPoll).toLocaleTimeString() + " (every " + POLL_S + " s)" : "-");
  setText("d-answer", model.answer + (model.failStreak ? ", " + model.failStreak + " failed in a row" : ""));
  setText("d-skew", (model.skew >= 0 ? "+" : "") + model.skew.toFixed(1) + " s from this computer");
  setText("d-pump", s ? String(s.pumpInRange) : "-");
  setText("d-phone", s ? String(s.phoneInRange) : "-");
  setText("d-reading", s && s.lastUpdate ? new Date(s.lastUpdate * 1000).toLocaleString() : "-");
  const la = s && s.lastAlarm;
  setText("d-alarm", la ? (la.faultId + " " + faultText(String(la.faultId)) + ", " + la.type + ", " + la.dateTime) : "-");
  setText("d-link", LINK_NAME[model.link]);
  const tr = model.trend;
  setText("d-trend", tr ? tr.sg.filter((v) => v).length + " readings, newest " + hhmm(tr.end) : "none");
  if ($("debug").open && rawShown !== model.raw) {   // only on change: keeps the scroll position
    rawShown = model.raw;
    let pretty = model.raw;
    try { pretty = JSON.stringify(JSON.parse(model.raw), null, 1); } catch (e) { /* show as is */ }
    setText("raw", pretty);
  }

  drawTrend();
  maybeSound();
}

// The chart is SVG built with createElementNS and textContent: no markup
// from a string, as the CSP and the rest of this page require.
const SVG_NS = "http://www.w3.org/2000/svg";
function svg(parent, name, attrs, text) {
  const e = document.createElementNS(SVG_NS, name);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (text !== undefined) e.textContent = text;
  parent.appendChild(e);
  return e;
}

function drawTrend() {
  const c = $("chart");
  c.replaceChildren();
  const W = 600, H = 240, x0 = 0, x1 = W - 44, y0 = 10, y1 = H - 28;
  const wend = now(), wstart = wend - TREND_SLOTS * TREND_STEP_S;
  const t = model.trend;
  const points = [];   // [time, value] of every slot in the window, 0 = none
  if (t) t.sg.forEach((v, i) => {
    const at = t.end - (TREND_SLOTS - 1 - i) * TREND_STEP_S;
    if (at >= wstart && at <= wend) points.push([at, v]);
  });
  const vals = points.map((p) => p[1]).filter((v) => v);
  const [ymin, ymax] = trendScale(vals.length ? Math.min(...vals) : 0, vals.length ? Math.max(...vals) : 0);
  const ym = (v) => y1 - (Math.min(Math.max(v, ymin), ymax) - ymin) * (y1 - y0) / (ymax - ymin);
  svg(c, "rect", { class: "band", x: x0, y: ym(HYPER_MGDL), width: x1 - x0, height: ym(HYPO_MGDL) - ym(HYPER_MGDL) });
  for (const [v, cls] of [[HYPO_MGDL, "lo"], [HYPER_MGDL, "hi"]]) {
    svg(c, "line", { class: cls, x1: x0, x2: x1, y1: ym(v), y2: ym(v) });
    svg(c, "text", { x: x1 + 6, y: ym(v) + 5 }, String(v));
  }
  // The target, always, finer than the thresholds; its label only where it
  // does not run into the 70's.
  svg(c, "line", { class: "target", x1: x0, x2: x1, y1: ym(TARGET_MGDL), y2: ym(TARGET_MGDL) });
  if (ym(HYPO_MGDL) - ym(TARGET_MGDL) >= 18)
    svg(c, "text", { x: x1 + 6, y: ym(TARGET_MGDL) + 5 }, String(TARGET_MGDL));
  if (ymax > TREND_Y_TOP_PLAIN && ym(HYPER_MGDL) - y0 >= 20) {
    // The top of the scale, what a high peak is read against.
    svg(c, "line", { class: "top", x1: x0, x2: x1, y1: y0, y2: y0 });
    svg(c, "text", { x: x1 + 6, y: y0 + 5 }, String(ymax));
  }
  svg(c, "line", { class: "axis", x1: x0, x2: x1, y1: y1, y2: y1 });

  const xm = (t) => x0 + (t - wstart) * (x1 - x0) / (wend - wstart);
  for (let h = (Math.floor(wstart / 3600) + 1) * 3600; h <= wend; h += 3600) {
    svg(c, "line", { class: "axis", x1: xm(h), x2: xm(h), y1: y1, y2: y1 + 5 });
    svg(c, "text", { x: xm(h), y: y1 + 22, "text-anchor": "middle" },
        String(new Date(h * 1000).getHours()).padStart(2, "0"));
  }

  // Broken wherever a slot has no reading; a lone reading is a dot. The big
  // dot marks the newest slot only when it has a reading, as on the display.
  let seg = [], last = null, drawn = 0;
  const flush = () => {
    if (seg.length > 1) svg(c, "polyline", { class: "line", points: seg.join(" ") });
    else if (seg.length === 1) { const [x, y] = seg[0].split(","); svg(c, "circle", { class: "pt", cx: x, cy: y, r: 2.5 }); }
    seg = [];
  };
  for (const [at, v] of points) {
    if (!v) { flush(); last = null; continue; }
    last = [xm(at), ym(v)];
    seg.push(last[0].toFixed(1) + "," + last[1].toFixed(1));
    drawn++;
  }
  flush();
  if (last) svg(c, "circle", { class: "dot", cx: last[0], cy: last[1], r: 6 });
  if (!drawn)
    svg(c, "text", { x: (x0 + x1) / 2, y: (y0 + y1) / 2, "text-anchor": "middle" },
        t ? "No readings in 3 h" : "No trend from proxy");
}

// ------------------------------------------------------------ sound

function beep(isAlarm) {
  if (!audio) return;
  const notes = isAlarm ? [[1800, 0.25], [0, 0.1], [1800, 0.25], [0, 0.1], [1800, 0.25], [0, 0.1], [1800, 0.4]]
                        : [[1200, 0.35], [0, 0.1], [1500, 0.35]];
  let t = audio.currentTime + 0.05;
  for (const [f, d] of notes) {
    if (f) {
      const o = audio.createOscillator(), g = audio.createGain();
      o.frequency.value = f;
      g.gain.setValueAtTime(0, t);
      g.gain.linearRampToValueAtTime(0.4, t + 0.005);
      g.gain.setValueAtTime(0.4, t + d - 0.005);
      g.gain.linearRampToValueAtTime(0, t + d);
      o.connect(g).connect(audio.destination);
      o.start(t);
      o.stop(t + d);
    }
    t += d;
  }
}

function maybeSound() {
  const a = alarm();
  if (!a) { alarmKey = null; return; }
  const t = Date.now() / 1000;
  if (a.key !== alarmKey || t - alarmSounded >= ALARM_REPEAT_S) {
    alarmKey = a.key;
    alarmSounded = t;
    beep(a.isAlarm);
  }
}

// ------------------------------------------------------------ start

$("sound").addEventListener("click", () => {
  // Browsers allow audio only after a click on the page.
  if (!audio) audio = new (window.AudioContext || window.webkitAudioContext)();
  audio.resume();
  $("sound").textContent = "Alarm sound on (click to test)";
  $("sound").classList.add("on");
  beep(true);
});
$("refresh").addEventListener("click", poll);
$("debug").addEventListener("toggle", draw);
// A tab in the background may be throttled or frozen; catch up on return.
document.addEventListener("visibilitychange", () => { if (!document.hidden) poll(); });
setInterval(draw, 1000);
poll();
