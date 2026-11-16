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
const NEED_LOGIN_MARKER = "Valid token required";

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
  lastPoll: null, answer: "-", raw: "",
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
  if ($("debug").open && rawShown !== model.raw) {   // only on change: keeps the scroll position
    rawShown = model.raw;
    let pretty = model.raw;
    try { pretty = JSON.stringify(JSON.parse(model.raw), null, 1); } catch (e) { /* show as is */ }
    setText("raw", pretty);
  }

  maybeSound();
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
