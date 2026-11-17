#!/usr/bin/env python3
###############################################################################
#
#  CareLink desktop monitor
#
#  The Core Ink display's screen in a window on the patient's (or a
#  caregiver's) own computer: a backup when no display is at hand, and a way
#  to see what the proxy serves without a board on the desk.
#
#  Only the Python standard library: on Windows and macOS a python.org install
#  is all it needs, on Linux also python3-tk. See desktop/README.md.
#
#    python3 minimed_desktop.py                     window, asks for the proxy once
#    python3 minimed_desktop.py --proxy carelink-01 window, this proxy (remembered)
#    python3 minimed_desktop.py --check             fetch once, print, exit
#    python3 minimed_desktop.py --file data.json    show a saved response
#    python3 minimed_desktop.py --debug             log every poll
#
#  The rules for what is shown - age, staleness, link faults, alarms - are the
#  firmware's (firmware/minimed-coreink), not those of the M5 monitor this
#  started from. Each has produced a plausible-looking wrong answer before;
#  the comments say which. Change them in both places or neither.
#
#  Based on minimed-mon-pc.py from Ondrej Wisniewski's M5 Minimed Monitor
#  (https://github.com/ondrej1024/m5-minimed-monitor), GPL-3.0. Rewritten by
#  Frederik, 2026.
#
#  This program is free software: you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by the Free
#  Software Foundation, either version 3 of the License, or (at your option)
#  any later version. It is distributed WITHOUT ANY WARRANTY; see LICENSE.md
#  in the root of this repository.
#
###############################################################################

import argparse
import atexit
import email.utils
import json
import math
import os
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from faults import FAULT_MAP, FAULT_TEXT, LOW_GLUCOSE_IDS, HIGH_GLUCOSE_IDS  # noqa: E402

VERSION = "1.0"

# ----------------------------------------------------------------- settings

DEFAULT_PORT = 8081
POLL_S = 60              # the pump uploads every 5 minutes; a minute keeps the lag small
TIMEOUT_S = 10           # a hung proxy must not freeze the window (the M5 version had none)
FAIL_STREAK_SHOWN = 3    # polls in a row without an answer before "No link to proxy"
STALE_READING_S = 20 * 60
ALARM_RECENCY_S = 15 * 60
ALARM_REPEAT_S = 5 * 60  # the display's cadence: one beep per 5-minute cycle
HYPO_MGDL = 70
HYPER_MGDL = 180
NEED_LOGIN_MARKER = "Valid token required"   # the proxy's own status string

LINK_OK, LINK_NO_SENSOR, LINK_PROXY_NO_DATA, LINK_PROXY_LOGIN, LINK_NO_PROXY = range(5)
LINK_TEXT = {
    LINK_NO_SENSOR: "No sensor data",
    LINK_PROXY_NO_DATA: "Proxy has no data",
    LINK_PROXY_LOGIN: "CareLink login needed",
    LINK_NO_PROXY: "No link to proxy",
}

DEBUG = False


def log(msg):
    if DEBUG:
        print(time.strftime("%H:%M:%S"), msg, file=sys.stderr, flush=True)


# ------------------------------------------------------------------- config

def config_path():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "carelink-monitor", "config.json")


def load_config():
    try:
        with open(config_path(), encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else {}
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    path = config_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=1)


def normalise_proxy(text):
    """'carelink-01', 'carelink-01:8081' or 'http://x:8081/' -> 'host:port'."""
    text = (text or "").strip()
    for prefix in ("http://", "https://"):
        if text.lower().startswith(prefix):
            text = text[len(prefix):]
    text = text.split("/")[0]
    if not text:
        return ""
    if ":" not in text:
        text = "%s:%d" % (text, DEFAULT_PORT)
    return text


# -------------------------------------------------------------------- fetch

class Reply:
    def __init__(self, status=0, body=b"", server_epoch=None, error=""):
        self.status = status
        self.body = body
        self.server_epoch = server_epoch
        self.error = error


def http_get(proxy, path):
    url = "http://%s/%s" % (proxy, path)
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT_S) as r:
            body = r.read()
            date = r.headers.get("Date")
            status = r.status
    except urllib.error.HTTPError as e:
        return Reply(status=e.code, error="HTTP %d" % e.code)
    except (urllib.error.URLError, OSError, ValueError) as e:
        return Reply(error=str(getattr(e, "reason", e)))
    server_epoch = None
    if date:
        try:
            server_epoch = email.utils.parsedate_to_datetime(date).timestamp()
        except (TypeError, ValueError):
            pass
    return Reply(status=status, body=body, server_epoch=server_epoch)


def proxy_wants_login(proxy):
    # Only asked when the data answer was empty. The two empty cases need
    # opposite responses: a proxy starting up fixes itself in a minute, an
    # expired token needs a person and a CAPTCHA, and otherwise sits dead
    # for days.
    r = http_get(proxy, "")
    return r.status == 200 and NEED_LOGIN_MARKER in r.body.decode("utf-8", "replace")


# ------------------------------------------------------------------ parsing

def fault_text(fault_id):
    canon = FAULT_MAP.get(fault_id, fault_id)
    return FAULT_TEXT.get(canon, "Unknown error code %s" % fault_id)


def parse_local(dt):
    """'yyyy-mm-ddThh:mm:ss...' -> epoch, reading the digits as LOCAL time.

    lastAlarm.dateTime carries the pump's local wall-clock time, whatever its
    suffix says (confirmed on the display: adding an offset showed it 2 hours
    ahead in CEST). Reading it as this computer's local time is right as long
    as the computer is in the patient's time zone. Returns 0 on any failure;
    0 must never pass for a recent alarm, which would also mean a false beep.
    """
    try:
        date, clock = dt.split("T")
        y, mo, d = (int(x) for x in date.split("-"))
        h, mi, s = (int(x) for x in clock[:8].split(":"))
        return time.mktime((y, mo, d, h, mi, s, 0, 0, -1))
    except (AttributeError, ValueError, OverflowError):
        return 0


def resolve_alarm(last_alarm, sg, now):
    """(text, local epoch, is_alarm, key) for a recent and still-current alarm, or None."""
    if not isinstance(last_alarm, dict):
        return None
    at = parse_local(last_alarm.get("dateTime"))
    if not at or at <= now - ALARM_RECENCY_S:
        return None
    fault_id = str(last_alarm.get("faultId") or "")
    if not fault_id:
        return None
    canon = FAULT_MAP.get(fault_id, fault_id)
    # A low/high notification goes stale the moment the reading recovers -
    # showing it then is worse than showing nothing.
    if sg > 0:
        if canon in LOW_GLUCOSE_IDS and sg > HYPO_MGDL:
            return None
        if canon in HIGH_GLUCOSE_IDS and sg < HYPER_MGDL:
            return None
    key = last_alarm.get("GUID") or "%s@%s" % (fault_id, last_alarm.get("dateTime"))
    return fault_text(fault_id), at, last_alarm.get("type") == "ALARM", key


def get(d, *keys, default=None):
    for k in keys:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return default if d is None else d


def num(v, default):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else default


def parse(doc):
    """The proxy's /carelink/nohistory object -> the fields the screen shows."""
    s = {}
    have = get(doc, "conduitInRange") is True and get(doc, "conduitMedicalDeviceInRange") is True
    s["haveData"] = have
    sg = num(get(doc, "lastSG", "sg"), 0)
    s["sg"] = int(sg) if sg > 0 else 0

    # Integer division: the value is Unix ms. The MicroPython builds divided
    # in float32 and were off by up to a minute.
    ms = num(get(doc, "lastConduitUpdateServerDateTime"), 0)
    s["lastUpdate"] = int(ms) // 1000 if ms > 0 else 0

    s["trend"] = "NONE"
    s["insulin"] = s["battery"] = s["reservoir"] = None
    s["sageHours"] = 255
    s["sensorState"] = ""
    if have:
        s["insulin"] = num(get(doc, "activeInsulin", "amount"), None)
        s["battery"] = num(get(doc, "pumpBatteryLevelPercent"), None)
        s["reservoir"] = num(get(doc, "reservoirRemainingUnits"), None)
        s["sageHours"] = num(get(doc, "sensorDurationHours"), 255)
        s["sensorState"] = str(get(doc, "sensorState", default=""))
        # The trend means something only with auto mode on.
        if get(doc, "therapyAlgorithmState", "autoModeShieldState", default="FEATURE_OFF") != "FEATURE_OFF":
            s["trend"] = str(get(doc, "lastSGTrend", default="NONE"))

    # A pump banner (suspend, BG required) outranks the generic status line.
    banner = ""
    sysmsg = get(doc, "systemStatusMessage", default="")
    if isinstance(sysmsg, str) and sysmsg and sysmsg != "NO_ERROR_MESSAGE":
        banner = sysmsg
    banners = get(doc, "pumpBannerState", default=[])
    if isinstance(banners, list) and banners and isinstance(banners[0], dict) and banners[0].get("type"):
        banner = str(banners[0]["type"])
    s["banner"] = banner.replace("_", " ")

    s["lastAlarm"] = get(doc, "lastAlarm")
    s["pumpInRange"] = get(doc, "conduitMedicalDeviceInRange")
    s["phoneInRange"] = get(doc, "conduitInRange")
    s["stats"] = tuple(num(get(doc, k), -1) for k in
                       ("timeInRange", "aboveHyperLimit", "belowHypoLimit", "averageSG"))
    return s


def poll(proxy, source_file=None):
    """One fetch. Returns (link, state-or-None, reply, raw text)."""
    if source_file:
        with open(source_file, "rb") as f:
            reply = Reply(status=200, body=f.read())
    else:
        reply = http_get(proxy, "carelink/nohistory")
    if reply.status != 200:
        return LINK_NO_PROXY, None, reply, ""
    raw = reply.body.decode("utf-8", "replace")
    try:
        doc = json.loads(raw)
    except ValueError:
        doc = None
    # A proxy holding nothing sends the JSON string "" (200, 2 bytes). An
    # answer, so not a link failure - but not a reading either, and it must
    # replace the old one: the screen may not keep showing a reading the
    # proxy can no longer stand behind.
    if not isinstance(doc, dict):
        link = LINK_PROXY_LOGIN if (not source_file and proxy_wants_login(proxy)) else LINK_PROXY_NO_DATA
        return link, None, reply, raw
    s = parse(doc)
    return (LINK_OK if s["sg"] > 0 else LINK_NO_SENSOR), s, reply, raw


# ----------------------------------------------------------------- the view

def age_text(age_s):
    m = int(age_s // 60)
    if m < 1:
        return "now"
    if m < 60:
        return "%d min" % m
    if m < 24 * 60:
        return "%d h %d min" % (m // 60, m % 60)
    if m < 48 * 60:
        return "1 day"
    return "%d days" % (m // (24 * 60))


TREND_ARROWS = {
    "UP_TRIPLE": "↑↑↑", "UP_DOUBLE": "↑↑", "UP": "↑",
    "NONE": "→",
    "DOWN": "↓", "DOWN_DOUBLE": "↓↓", "DOWN_TRIPLE": "↓↓↓",
}


class Model:
    """Everything the screen needs, updated by polls and read by the clock tick."""

    def __init__(self):
        self.state = None          # last parsed answer, None = none (or no longer valid)
        self.link = LINK_OK        # the link state SHOWN (after smoothing)
        self.fail_streak = 0
        self.skew = 0.0            # proxy clock minus this computer's clock
        self.last_good_sg = 0
        self.last_poll = None      # local time of the last poll, for the status line
        self.raw = ""
        self.reply = None

    def now(self):
        # The proxy's clock, not this computer's: a PC that is minutes off
        # would otherwise misjudge the reading's age and every alarm window.
        return time.time() + self.skew

    def apply(self, link, state, reply, raw):
        self.last_poll = time.time()
        self.reply = reply
        if reply.server_epoch:
            self.skew = reply.server_epoch - time.time()
        if link == LINK_NO_PROXY:
            # One missed poll is a hiccup: keep the last reading, whose age
            # keeps counting on screen. Several in a row get a banner.
            self.fail_streak += 1
            if self.fail_streak >= FAIL_STREAK_SHOWN:
                self.link = LINK_NO_PROXY
            return
        self.fail_streak = 0
        self.raw = raw
        self.link = link
        self.state = state
        if state and state["sg"] > 0:
            self.last_good_sg = state["sg"]

    def age(self):
        if not self.state or not self.state["lastUpdate"]:
            return None
        return max(0.0, self.now() - self.state["lastUpdate"])

    def alarm(self):
        if not self.state or self.link == LINK_NO_PROXY:
            return None
        return resolve_alarm(self.state["lastAlarm"], self.state["sg"], self.now())

    def banner(self):
        """(text, kind) with kind in alarm/link/stale/pump, or ('', None).

        The display's order: an alarm outranks everything; a link fault or a
        stale reading outranks the pump banner, which is exactly as old as
        the reading when the link is down.
        """
        a = self.alarm()
        if a:
            return "%s (%s)" % (a[0], time.strftime("%H:%M", time.localtime(a[1]))), "alarm"
        age = self.age()
        if self.link == LINK_NO_SENSOR and age is not None:
            # With its age: "no sensor data" is not proof the proxy is
            # fetching now. Minutes read as routine, hours do not.
            return "%s (%s)" % (LINK_TEXT[self.link], age_text(age)), "link"
        if self.link != LINK_OK:
            return LINK_TEXT[self.link], "link"
        if age is not None and age >= STALE_READING_S:
            return "No new reading for %s" % age_text(age), "stale"
        if self.state and self.state["banner"]:
            return self.state["banner"], "pump"
        return "", None

    def summary(self):
        """Plain text of what the window would show, for --check and the log."""
        lines = []
        s = self.state
        sg = s["sg"] if s and self.link != LINK_NO_PROXY else 0
        lines.append("glucose   %s mg/dL %s" % (sg or "---", TREND_ARROWS.get(s["trend"], "") if s and sg else ""))
        text, kind = self.banner()
        lines.append("banner    %s%s" % (text or "-", " [%s]" % kind if kind else ""))
        age = self.age()
        lines.append("updated   %s" % (("now" if age < 60 else "%s ago" % age_text(age)) if age is not None else "no data"))
        if s:
            lines.append("insulin   %s U" % ("%.1f" % s["insulin"] if s["insulin"] is not None else "--"))
            lines.append("pump      battery %s %%, reservoir %s U, sensor %s"
                         % (s["battery"] if s["battery"] is not None else "--",
                            "%.0f" % s["reservoir"] if s["reservoir"] is not None else "--",
                            sensor_text(s) or "--"))
            lines.append("comms     pump-phone %s, phone-cloud %s" % (s["pumpInRange"], s["phoneInRange"]))
        lines.append("clock     proxy is %+.0f s from this computer" % self.skew)
        return "\n".join(lines)


def sensor_text(s):
    if s["sensorState"] == "CHANGE_SENSOR":
        return "change"
    h = s["sageHours"]
    if h == 255 or h is None:
        return ""
    return "%d d" % round(h / 24) if h > 9 else "%d h" % h


# -------------------------------------------------------------------- sound

class Sound:
    """Two tones written to WAV files once, played the way each OS allows.

    Generated rather than shipped, so the program stays one directory of
    text files. The terminal bell is the last resort: quiet, but not silent.
    """

    def __init__(self, tk_root=None):
        self.root = tk_root
        self.dir = tempfile.mkdtemp(prefix="carelink-monitor-")
        atexit.register(shutil.rmtree, self.dir, True)
        self.files = {
            "alarm": self._write("alarm.wav", [(1800, 0.25), (0, 0.1)] * 3 + [(1800, 0.4)]),
            "alert": self._write("alert.wav", [(1200, 0.35), (0, 0.1), (1500, 0.35)]),
        }
        self.player = self._find_player()

    def _write(self, name, notes, rate=22050):
        path = os.path.join(self.dir, name)
        frames = bytearray()
        for freq, dur in notes:
            n = int(rate * dur)
            for i in range(n):
                # 5 ms ramps at both ends of each note: no clicks.
                env = min(1.0, i / (rate * 0.005), (n - i) / (rate * 0.005))
                v = math.sin(2 * math.pi * freq * i / rate) * env if freq else 0.0
                frames += struct.pack("<h", int(v * 26000))
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(bytes(frames))
        return path

    def _find_player(self):
        if sys.platform == "win32":
            return "winsound"
        candidates = ["afplay"] if sys.platform == "darwin" else ["paplay", "pw-play", "aplay"]
        for c in candidates:
            if shutil.which(c):
                return c
        return None

    def play(self, kind):
        path = self.files["alarm" if kind == "alarm" else "alert"]
        log("sound: %s via %s" % (kind, self.player or "bell"))
        try:
            if self.player == "winsound":
                import winsound
                winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC)
                return
            if self.player:
                args = [self.player, path] if self.player != "aplay" else ["aplay", "-q", path]
                subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
        except Exception as e:
            log("sound failed: %s" % e)
        if self.root is not None:
            self.root.bell()

    def describe(self):
        return self.player or "the system bell only (no audio player found)"


# ------------------------------------------------------------------- window

COLORS = {
    "bg": "#101418", "fg": "#ffffff", "dim": "#9aa4ad",
    "low": "#d32f2f", "high": "#f9a825", "high_fg": "#101418",
    "alarm": "#d32f2f", "link": "#ef6c00", "stale": "#ef6c00", "pump": "#37474f",
}


class Window:
    def __init__(self, args, cfg):
        import tkinter as tk
        from tkinter import font as tkfont
        self.tk = tk
        self.args = args
        self.cfg = cfg
        self.model = Model()
        self.results = queue.Queue()
        self.wake = threading.Event()
        self.alarm_key = None
        self.alarm_sounded = 0.0

        # The class name ties the window to its menu launcher (install-linux.sh).
        self.root = root = tk.Tk(className="carelink-monitor")
        root.title("CareLink monitor")
        root.configure(bg=COLORS["bg"])
        root.minsize(420, 300)
        root.geometry("520x340")
        self.sound = Sound(root)

        family = "Segoe UI" if sys.platform == "win32" else ("Helvetica Neue" if sys.platform == "darwin" else "DejaVu Sans")
        if family not in tkfont.families(root):
            family = "Helvetica"
        f = lambda size, weight="normal": tkfont.Font(root=root, family=family, size=size, weight=weight)
        self.font_sg = f(72, "bold")
        self.font_trend = f(40, "bold")
        self.font_banner = f(15, "bold")
        self.font_text = f(13)
        self.font_small = f(10)

        self._menu()

        bg, fg, dim = COLORS["bg"], COLORS["fg"], COLORS["dim"]
        top = self.top = tk.Frame(root, bg=bg)
        top.pack(fill="x", padx=16, pady=(12, 0))
        self.sg = tk.Label(top, text="---", font=self.font_sg, bg=bg, fg=fg, anchor="w")
        self.sg.pack(side="left")
        self.trend = tk.Label(top, text="", font=self.font_trend, bg=bg, fg=fg, anchor="w")
        self.trend.pack(side="left", padx=(12, 0))

        unit_row = tk.Frame(root, bg=bg)
        unit_row.pack(fill="x", padx=16)
        tk.Label(unit_row, text="mg/dL", font=self.font_text, bg=bg, fg=dim).pack(side="left")
        # While there is no reading, what it last WAS - bracketed, so it can
        # never pass for a current one at a glance.
        self.prev = tk.Label(unit_row, text="", font=self.font_text, bg=bg, fg=dim)
        self.prev.pack(side="right")

        self.banner = tk.Label(root, text="", font=self.font_banner, bg=bg, fg=fg,
                               wraplength=480, justify="center", pady=6)
        self.banner.pack(fill="x", padx=16, pady=(8, 0))
        root.bind("<Configure>", lambda e: self.banner.configure(wraplength=max(200, root.winfo_width() - 40)))

        info = tk.Frame(root, bg=bg)
        info.pack(fill="x", padx=16, pady=(10, 0))
        self.insulin = self._kv(info, "Act. insulin", 0)
        self.battery = self._kv(info, "Pump battery", 1)
        self.reservoir = self._kv(info, "Reservoir", 2)
        self.sensor = self._kv(info, "Sensor", 3)
        for c in range(4):
            info.columnconfigure(c, weight=1)

        bottom = tk.Frame(root, bg=bg)
        bottom.pack(fill="x", side="bottom", padx=16, pady=(0, 10))
        self.stats = tk.Label(bottom, text="", font=self.font_small, bg=bg, fg=dim, anchor="w")
        self.stats.pack(side="top", fill="x")
        row = tk.Frame(bottom, bg=bg)
        row.pack(side="top", fill="x")
        self.updated = tk.Label(row, text="Updated: no data", font=self.font_text, bg=bg, fg=fg, anchor="w")
        self.updated.pack(side="left")
        self.clock = tk.Label(row, text="--:--", font=self.font_text, bg=bg, fg=fg)
        self.clock.pack(side="right")

        threading.Thread(target=self._poller, daemon=True).start()
        root.after(200, self._tick)

    def _kv(self, parent, label, col):
        tk = self.tk
        tk.Label(parent, text=label, font=self.font_small, bg=COLORS["bg"], fg=COLORS["dim"]).grid(row=0, column=col, sticky="w")
        v = tk.Label(parent, text="--", font=self.font_text, bg=COLORS["bg"], fg=COLORS["fg"])
        v.grid(row=1, column=col, sticky="w")
        return v

    def _menu(self):
        tk = self.tk
        bar = tk.Menu(self.root)
        m = tk.Menu(bar, tearoff=0)
        m.add_command(label="Refresh now", command=self.wake.set)
        self.on_top = tk.BooleanVar(value=bool(self.cfg.get("on_top")))
        m.add_checkbutton(label="Keep on top", variable=self.on_top, command=self._set_on_top)
        m.add_command(label="Test sound", command=lambda: self.sound.play("alarm"))
        m.add_separator()
        m.add_command(label="Proxy address…", command=self._ask_proxy)
        m.add_command(label="Show proxy answer…", command=self._show_raw)
        m.add_command(label="About…", command=self._about)
        m.add_separator()
        m.add_command(label="Quit", command=self.root.destroy)
        bar.add_cascade(label="Monitor", menu=m)
        self.root.configure(menu=bar)
        self._set_on_top()

    def _set_on_top(self):
        self.root.attributes("-topmost", self.on_top.get())
        if self.cfg.get("on_top") != self.on_top.get():
            self.cfg["on_top"] = self.on_top.get()
            self._save()

    def _save(self):
        if not self.args.file:
            try:
                save_config(self.cfg)
            except OSError as e:
                log("could not save settings: %s" % e)

    def _ask_proxy(self):
        from tkinter import simpledialog
        text = simpledialog.askstring(
            "Proxy address",
            "Name or address of the CareLink proxy (the Raspberry Pi),\n"
            "for example carelink-01 or 192.168.1.20:", parent=self.root,
            initialvalue=self.cfg.get("proxy", ""))
        proxy = normalise_proxy(text)
        if proxy:
            self.cfg["proxy"] = proxy
            self._save()
            self.wake.set()

    def _show_raw(self):
        tk = self.tk
        w = tk.Toplevel(self.root)
        w.title("Proxy answer")
        t = tk.Text(w, width=90, height=32, font=("Courier", 10))
        t.pack(fill="both", expand=True)
        m = self.model
        head = "proxy %s, last poll %s, %s\n\n" % (
            self.args.file or self.cfg.get("proxy"),
            time.strftime("%H:%M:%S", time.localtime(m.last_poll)) if m.last_poll else "-",
            ("status %d" % m.reply.status if m.reply and m.reply.status else
             "error: %s" % (m.reply.error if m.reply else "-")))
        try:
            body = json.dumps(json.loads(m.raw), indent=1, sort_keys=True)
        except ValueError:
            body = m.raw or "(nothing)"
        t.insert("1.0", head + m.summary() + "\n\n" + body)
        t.configure(state="disabled")

    def _about(self):
        from tkinter import messagebox
        messagebox.showinfo(
            "About", "CareLink desktop monitor %s\n\nProxy: %s\nSound: %s\nSettings: %s\n\n"
            "A backup for the display, not a medical device. Keep this window "
            "open and the computer awake; a sleeping computer shows nothing."
            % (VERSION, self.args.file or self.cfg.get("proxy"), self.sound.describe(), config_path()),
            parent=self.root)

    # -- polling, off the Tk thread: a slow proxy must never block the window

    def _poller(self):
        while True:
            proxy = self.cfg.get("proxy")
            if proxy or self.args.file:
                try:
                    result = poll(proxy, self.args.file)
                except Exception as e:   # never let the poller die silently
                    result = (LINK_NO_PROXY, None, Reply(error="internal: %s" % e), "")
                self.results.put(result)
            self.wake.wait(POLL_S)
            self.wake.clear()

    def _tick(self):
        try:
            while True:
                link, state, reply, raw = self.results.get_nowait()
                self.model.apply(link, state, reply, raw)
                log("poll: link=%d status=%s %s" % (link, reply.status, reply.error))
                if DEBUG:
                    log(self.model.summary())
        except queue.Empty:
            pass
        try:
            self._draw()
            self._maybe_sound()
        except Exception as e:
            # Visible, not swallowed: a screen that silently stops updating
            # looks exactly like a working one.
            self.banner.configure(text="Display error: %s" % e, bg=COLORS["alarm"])
            log("draw failed: %r" % e)
        self.root.after(1000, self._tick)

    def _maybe_sound(self):
        a = self.model.alarm()
        if not a:
            self.alarm_key = None
            return
        now = time.time()
        if a[3] != self.alarm_key or now - self.alarm_sounded >= ALARM_REPEAT_S:
            self.alarm_key = a[3]
            self.alarm_sounded = now
            self.sound.play("alarm" if a[2] else "alert")

    def _draw(self):
        m, s, C = self.model, self.model.state, COLORS
        if not self.cfg.get("proxy") and not self.args.file:
            self.banner.configure(text="No proxy set: Monitor → Proxy address", bg=C["link"])
            return
        current = s is not None and m.link != LINK_NO_PROXY
        sg = s["sg"] if current else 0

        bg, fg = C["bg"], C["fg"]
        if sg and sg < HYPO_MGDL:
            bg = C["low"]
        elif sg and sg > HYPER_MGDL:
            bg, fg = C["high"], C["high_fg"]
        self.top.configure(bg=bg)
        self.sg.configure(text=str(sg) if sg else "---", bg=bg, fg=fg)
        self.trend.configure(text=TREND_ARROWS.get(s["trend"], "→") if sg else "", bg=bg, fg=fg)
        self.prev.configure(text="[%d]" % m.last_good_sg if not sg and m.last_good_sg else "")

        text, kind = m.banner()
        self.banner.configure(text=text, bg=C[kind] if kind else C["bg"])

        def show(v, fmt):
            return fmt % v if current and v is not None else "--"
        self.insulin.configure(text=show(s and s["insulin"], "%.1f U"))
        self.battery.configure(text=show(s and s["battery"], "%d %%"))
        self.reservoir.configure(text=show(s and s["reservoir"], "%.0f U"))
        self.sensor.configure(text=(sensor_text(s) or "--") if current else "--")

        st = s["stats"] if s else (-1, -1, -1, -1)
        self.stats.configure(text="24 h: %d%% in range, %d%% above, %d%% below, average %d" % st
                             if current and min(st) >= 0 else "")

        age = m.age()
        if age is None:
            self.updated.configure(text="Updated: no data")
        else:
            self.updated.configure(text="Updated now" if age < 60 else "Updated %s ago" % age_text(age))
        self.clock.configure(text=time.strftime("%H:%M", time.localtime(m.now())))

    def run(self):
        if not self.cfg.get("proxy") and not self.args.file:
            self.root.after(300, self._ask_proxy)
        self.root.mainloop()


# --------------------------------------------------------------------- main

def main():
    global DEBUG
    p = argparse.ArgumentParser(description="CareLink desktop monitor")
    p.add_argument("--proxy", help="proxy name or address, optionally :port (remembered)")
    p.add_argument("--check", action="store_true", help="fetch once, print what would be shown, exit")
    p.add_argument("--file", help="show a saved /carelink/nohistory answer instead of polling")
    p.add_argument("--debug", action="store_true", help="log every poll to the terminal")
    args = p.parse_args()
    DEBUG = args.debug

    cfg = load_config()
    if args.proxy:
        cfg["proxy"] = normalise_proxy(args.proxy)
        if not args.check:
            save_config(cfg)

    if args.check:
        if not cfg.get("proxy") and not args.file:
            sys.exit("no proxy known: pass --proxy")
        link, state, reply, raw = poll(cfg.get("proxy"), args.file)
        m = Model()
        m.fail_streak = FAIL_STREAK_SHOWN - 1   # one try is all --check gets
        m.apply(link, state, reply, raw)
        print("proxy     %s (%s)" % (args.file or cfg["proxy"],
                                     "status %d" % reply.status if reply.status else reply.error))
        print(m.summary())
        a = m.alarm()
        print("alarm     %s" % ("%s -> %s" % (a[0], "alarm sound" if a[2] else "alert sound") if a else "-"))
        sys.exit(0 if link in (LINK_OK, LINK_NO_SENSOR) else 2)

    Window(args, cfg).run()


if __name__ == "__main__":
    main()
