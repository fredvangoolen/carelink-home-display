###############################################################################
#  
#  Carelink Client 2 Proxy
#  
#  Description:
#
#    This program periodically downloads the available data from the 
#    Medtronic Carelink API. Then this data is provided via a simple
#    REST API to local clients:
#
#    Send a GET request to the following URI: 
#      http://<serveraddr>:8081/carelink/          # all Carelink data
#      http://<serveraddr>:8081/carelink/nohistory # no history data
#      http://<serveraddr>:8081/carelink/fallback  # the display's plan B
#                                                  # hotspot, if this Pi runs one
#      http://<serveraddr>:8081/carelink/trend     # the last 3 h of readings,
#                                                  # compact, for the trend screen
#
#  Author:
#
#    Ondrej Wisniewski (ondrej.wisniewski *at* gmail.com)
#  
#  Changelog:
#
#    08/06/2021 - Initial public release
#    27/07/2021 - Add logging, bug fixes
#    06/02/2022 - Download new data as soon as it is available
#    08/02/2022 - Fix HTTP API
#    24/05/2023 - Add patient parameter
#    12/10/2023 - Replace login parameters with initial token
#    27/10/2023 - Add Web GUI to insert auth token
#    03/01/2024 - Porting to Carelink Client 2
#    11/04/2024 - Handle reconnection in case of network error
#    17/01/2025 - Adapt get_essential_data() to new data format
#    04/07/2026 - Adapt time until next reading calculation to new data format
#
#  Copyright 2021-2026, Ondrej Wisniewski
#
#  Modified:
#
#    This is not the original file. It was taken from
#    https://github.com/ondrej1024/carelink-python-client in 2026 and changed
#    by Frederik in October and November 2026 for carelink-home-display
#    (https://github.com/fredvangoolen/carelink-home-display): among other
#    things the /carelink/fallback endpoint, a token accepted by POST only
#    while one is needed, no cross-origin access, the handling of clock
#    jumps and of error answers from CareLink, a status page that says when
#    the data stopped and why, and a quick retry after a network outage.
#    That repository's history has each change with its date.
#
#  Modifications copyright 2026, Frederik
#
###############################################################################

import carelink_client2
import argparse
import time
import calendar
import json
import os
import sys
import signal
import threading 
import logging as log
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http import HTTPStatus
from urllib.parse import parse_qs, urlsplit
import html


VERSION = "1.3"

# Logging config
FORMAT = '[%(asctime)s:%(levelname)s] %(message)s'
log.basicConfig(format=FORMAT, datefmt='%Y-%m-%d %H:%M:%S', level=log.INFO)

# HTTP server settings
HOSTNAME = "0.0.0.0"
PORT     = 8081
GUIURL   = ""

# A real logindata.json is about 2.6KB. Anything far beyond that is not a
# token, and reading an unbounded body into memory on request is a free way
# to knock the proxy over.
MAX_POST_BYTES = 16 * 1024
APIURL   = "carelink"
OPT_NOHISTORY = "nohistory"
OPT_FALLBACK  = "fallback"
OPT_TREND     = "trend"

# The trend screen's window: 36 readings, 5 minutes apart, 3 hours. The
# display has no room for the full sgs list (about 38 KB of a 119 KB answer
# on a board whose largest free block is about 55 KB), so the proxy cuts it
# down to one small integer per slot.
TREND_STEP_S = 300
TREND_SLOTS  = 36

# The display's plan B: the private hotspot this Pi runs next to its own
# WiFi (appliance/travel-ap). Serving its name and password here lets a
# display learn it over the household network on its own, instead of
# someone typing a 24-character random password into a phone. The LAN is
# already the trust boundary (see do_GET), and the hotspot itself only
# reaches this same port. Read on every request, so a re-provisioned
# password is picked up without a restart; absent file = no hotspot = 404.
FALLBACK_SSID    = "carelink-travel"
FALLBACK_PSKFILE = "/etc/carelink/travel-ap.psk"

# The live display page, for debugging and a quick look from any browser on
# the network; not a monitor (it cannot sound reliably and dies with its tab).
# A fixed table, never a path joined from the request: nothing else in that
# directory, or outside it, can be served by this. Read on every request, so
# an upgraded page needs no restart.
DISPLAY_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "display")
DISPLAY_FILES = {
   "display":            ("index.html",  "text/html; charset=utf-8"),
   "display/display.js": ("display.js",  "text/javascript; charset=utf-8"),
   "display/faults.js":  ("faults.js",   "text/javascript; charset=utf-8"),
   "display/display.css": ("display.css", "text/css; charset=utf-8"),
}
# The page shows text that comes from CareLink (fault and status strings).
# Script running on THIS origin would pass the Origin check that guards the
# token POST, so: no inline script, no other origin, nothing framed, and the
# page itself inserts all data as text, never as HTML.
DISPLAY_HEADERS = {
   "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; "
                              "connect-src 'self'; img-src 'self'; base-uri 'none'; "
                              "form-action 'none'; frame-ancestors 'none'",
   "X-Frame-Options": "DENY",
   "X-Content-Type-Options": "nosniff",
   "Referrer-Policy": "no-referrer",
   "Cache-Control": "no-store",
}

UPDATE_INTERVAL = 300
RETRY_INTERVAL  = 120

# Token handling
TOKENFILE = "logindata.json"
wait_for_params = True
# A failed start is retried with the stored token, not only on a new one.
# Doubling from the first value up to the cap - but only when CareLink
# answered and refused. When it could not be reached at all (no network,
# DNS, a 5xx) the retry stays at the first value: that costs CareLink
# nothing, and backing off there kept a Pi on 30-minute waits after its
# network came back (carelink-01, 2026-11-15).
INIT_RETRY_FIRST_S = 60
INIT_RETRY_MAX_S   = 1800

# Status messages
STATUS_INIT     = "Initialization"
STATUS_DO_LOGIN = "Performing login"
STATUS_LOGIN_OK = "Login successful"
STATUS_NEED_TKN = "Valid token required"
STATUS_NO_DATA  = "Logged in, but no data from CareLink"
g_status = STATUS_INIT
# One line under the status: since when, and why. The status alone said
# "Login successful" through five hours of refused refreshes (the reference Pi,
# 2026-11-15), while the displays got nothing.
g_detail = ""
g_last_data = None          # wall-clock time of the last good download

recentData = None
verbose = False


#################################################
# The signal handler for the TERM signal
#################################################
def on_sigterm(signum, frame):
   # TODO: cleanup (if any)
   log.debug("Exiting in sigterm")
   sys.exit()


#################################################
# Get only essential data from json
#################################################
def get_essential_data(data):
   mydata = ""
   # CareLink answers a failed request with a body that has no patientData.
   # That is no data, like None: answering nothing at all instead (a KeyError
   # here) left the display waiting 30 s for a reply, twice (2026-11-14).
   if isinstance(data, dict) and isinstance(data.get("patientData"), dict):
      mydata = data["patientData"].copy()
      try:
         del mydata["sgs"]
      except (KeyError,TypeError) as e:
         pass
      try:
         del mydata["markers"]
      except (KeyError,TypeError) as e:
         pass
      try:
         del mydata["limits"]
      except (KeyError,TypeError) as e:
         pass
      try:
         del mydata["notificationHistory"]
      except (KeyError,TypeError) as e:
         pass
   return mydata
   

#################################################
# The display's fallback hotspot, or None
#################################################
def fallback_network():
   try:
      with open(fallback_pskfile) as f:
         psk = f.read().strip()
   except OSError:
      return None
   # WPA2-PSK passphrase bounds; anything else is not a usable password.
   if not 8 <= len(psk) <= 63:
      return None
   return {"ssid": FALLBACK_SSID, "psk": psk}


#################################################
# The last 3 hours of readings, for the trend screen
#################################################
def parse_local(stamp):
   # sgs timestamps are LOCAL wall-clock time with no offset, like
   # lastAlarm.dateTime (2026-08-15: a reading stamped 21:09:00 arrived with
   # lastConduitUpdateServerDateTime 19:15 UTC, in CEST). They are kept
   # local here and on the display, which reads them with the same local
   # clock as the alarm time, so one rule decides both. Naive arithmetic:
   # across a DST change the window shows an hour's gap or overlap, for
   # three hours, twice a year.
   try:
      return calendar.timegm(time.strptime(stamp[:19], "%Y-%m-%dT%H:%M:%S"))
   except (TypeError, ValueError):
      return None


def get_trend_data(data):
   # {"end": "<local timestamp of the newest reading>", "step": 300,
   #  "sg": [36 values, oldest first, 0 = no reading in that slot]}
   # or None when there are no readings to show.
   if not (isinstance(data, dict) and isinstance(data.get("patientData"), dict)):
      return None
   sgs = data["patientData"].get("sgs")
   if not isinstance(sgs, list):
      return None
   points = []
   for e in sgs:
      if not isinstance(e, dict):
         continue
      t = parse_local(e.get("timestamp"))
      sg = e.get("sg")
      # 0 is how CareLink marks a slot with no value (warm-up, lost signal).
      if t is None or not isinstance(sg, (int, float)) or sg <= 0:
         continue
      points.append((t, int(sg), e["timestamp"][:19]))
   if not points:
      return None
   end_t, _, end_stamp = max(points)
   slots = [0] * TREND_SLOTS
   for t, sg, _ in points:
      back = round((end_t - t) / TREND_STEP_S)
      if 0 <= back < TREND_SLOTS:
         slots[TREND_SLOTS - 1 - back] = sg
   return {"end": end_stamp, "step": TREND_STEP_S, "sg": slots}


def since(t):
   return time.strftime("%Y-%m-%d %H:%M", time.localtime(t)) if t else "this start"


def webgui(status,action=None,error=""):
   head =  '<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 4.01//EN" "http://www.w3.org/TR/html4/strict.dtd"> \n \
            <html><head><title>Carelink Client 2 Proxy</title> \n \
            <style></style> \n \
            </head> \n \
            <body><table style="text-align: left; width: 460px; background-color: #2196F3; font-family: Helvetica,Arial,sans-serif; font-weight: bold; color: white;" border="0" cellpadding="2" cellspacing="2"> \n \
            <tbody><tr><td> \n \
            <span style="vertical-align: top; font-size: 48px;">Carelink Client 2</span><br> \n \
            </td></tr></tbody></table><br> \n'


   body =  '<table style="text-align: left; width: 460px; background-color: white; font-family: Helvetica,Arial,sans-serif; font-size: 18px;" border="0" cellpadding="2" cellspacing="3"><tbody> \n \
            <tr style="font-size: 18px; font-weight: bold; background-color: lightgrey"> \n \
            <td style="width: 200px;">Status</td> \n \
            <td style="background-color: white;"></td><td style="background-color: white;"></td></tr> \n \
            <tr style="vertical-align: top; background-color: rgb(230, 230, 255);"> \n \
            <td style="width: 300px;">%s</td> \n \
            <tr style="vertical-align: top; font-size: 14px;"> \n \
            <td style="width: 300px;">%s</td> \n \
            </tbody></table><br> \n' % (status, html.escape(g_detail))

   form = ""
   if action is not None:
      form = '<table style="text-align: left; width: 460px; background-color: white; font-family: Helvetica,Arial,sans-serif; font-size: 14px;" border="0" cellpadding="2" cellspacing="3"><tbody> \n \
            <tr><td>Paste the full contents of a freshly generated <code>logindata.json</code> \n \
            (produced by running <code>carelink_carepartner_api_login.py</code> on a machine with a browser) below:</td></tr> \n \
            <tr><td style="color: red;">%s</td></tr> \n \
            <tr><td> \n \
            <form method="POST" action="/%s"> \n \
            <textarea name="tokendata" rows="12" cols="60"></textarea><br> \n \
            <input type="submit" value="Save token"> \n \
            </form> \n \
            </td></tr> \n \
            </tbody></table><br> \n' % (error, action)

   tail =  '<span style="font-size: 16px; color: red; font-family: Helvetica,Arial,sans-serif;"></span><br> \n \
            <table style="text-align: left; width: 460px; background-color: #2196F3;" border="0" cellpadding="2" cellspacing="2"><tbody> \n \
            <tr><td style="vertical-align: top; text-align: center;"> \n \
            <span style="font-family: Helvetica,Arial,sans-serif; color: white;"><a style="color: white;" href=/display>live display</a> | <a style="color: white;" href=https://github.com/fredvangoolen/carelink-home-display>carelink-home-display</a> proxy | version %s | based on <a style="color: white;" href=https://github.com/ondrej1024/carelink-python-client>carelink_client2_proxy</a></span></td></tr> \n \
            </tbody></table></body></html>' % VERSION

   # Not called "html": that name is the module escaping g_detail above.
   page = head + body + form + tail
   return page


#################################################
# Validate and save token data posted from the web GUI,
# then wake up the main loop so it re-inits the client
#################################################
def save_params(tokendata):
   global wait_for_params

   token_data = json.loads(tokendata)
   required_fields = ["access_token", "refresh_token", "scope", "client_id"]
   for f in required_fields:
      if f not in token_data:
         raise ValueError("field %s is missing from pasted token data" % f)

   with open(tokenfile, 'w') as f:
      json.dump(token_data, f, indent=4)
   log.info("New token data saved to %s via web GUI" % tokenfile)

   wait_for_params = False


#################################################
# HTTP server methods
#################################################
class MyServer(BaseHTTPRequestHandler):
   
   def log_message(self, format, *args):
      #Disable logging
      pass

   def _reply(self, status_code, content_type, response, headers=None):
      # Deliberately NO Access-Control-Allow-Origin. It used to be "*", which
      # let any web page open in any browser on the same network read the
      # full patient record cross-origin - no compromise needed, just a bad
      # page in a household browser. Nothing that consumes this proxy is a
      # browser: the displays fetch it directly, and CORS does not apply to
      # them at all.
      self.send_response(status_code)
      self.send_header("Content-type", content_type)
      for name, value in (headers or {}).items():
         self.send_header(name, value)
      self.end_headers()
      try:
         self.wfile.write(bytes(response, "utf-8"))
      except BrokenPipeError:
         pass

   def _same_origin(self):
      # A browser attaches Origin to every cross-site POST, so a request whose
      # Origin is not this proxy's own host:port was sent by some OTHER page.
      # That is the case that matters: during an outage, the person with the
      # recovery page open is exactly the person whose browser another tab
      # could use to submit a token. A non-browser client sends no Origin;
      # there is nothing for a web page to forge there, so it is allowed.
      origin = self.headers.get("Origin")
      if origin is None:
         return True
      host = (self.headers.get("Host") or "").lower()
      return origin != "null" and urlsplit(origin).netloc.lower() == host

   def do_GET(self):
      # Reads are open to the LAN by design: the displays that consume this
      # cannot authenticate, and the household network is the trust boundary.
      # What IS guarded is the one write - see do_POST.
      log.debug("received client GET request from %s" % (self.address_string()))
      #print(self.path)
      
      # Check request path
      if self.path.strip("/") == APIURL:
         # Get latest Carelink data (complete)
         response = json.dumps(recentData)
         status_code = HTTPStatus.OK
         content_type = "application/json"
         #print("All data requested")
      elif self.path.strip("/") == APIURL+'/'+OPT_NOHISTORY:
         # Get latest Carelink data without history
         response = json.dumps(get_essential_data(recentData))
         status_code = HTTPStatus.OK
         content_type = "application/json"
         #print("Only essential data requested")
      elif self.path.strip("/") == APIURL+'/'+OPT_TREND:
         # 404 when there is nothing to draw, so a display treats "no
         # readings" and "an older proxy without this URL" the same way.
         trend = get_trend_data(recentData)
         if trend:
            response = json.dumps(trend, separators=(",", ":"))
            status_code = HTTPStatus.OK
            content_type = "application/json"
         else:
            response = ""
            status_code = HTTPStatus.NOT_FOUND
            content_type = "text/html"
      elif self.path.strip("/") == APIURL+'/'+OPT_FALLBACK:
         # The display's plan B hotspot. Never logged: it carries a password.
         fallback = fallback_network()
         if fallback:
            response = json.dumps(fallback)
            status_code = HTTPStatus.OK
            content_type = "application/json"
         else:
            response = ""
            status_code = HTTPStatus.NOT_FOUND
            content_type = "text/html"
      elif self.path.strip("/") in DISPLAY_FILES:
         name, content_type = DISPLAY_FILES[self.path.strip("/")]
         try:
            with open(os.path.join(DISPLAY_DIR, name), encoding="utf-8") as f:
               response = f.read()
            status_code = HTTPStatus.OK
         except OSError as e:
            log.warning("display page: %s" % e)
            response = ""
            status_code = HTTPStatus.NOT_FOUND
         self._reply(status_code, content_type, response, DISPLAY_HEADERS)
         return
      elif self.path == "/":
         # Show web GUI
         if g_status == STATUS_NEED_TKN:
            response = webgui(status=g_status, action=GUIURL)
         else:
            response = webgui(status=g_status)
         status_code = HTTPStatus.OK
         content_type = "text/html"
         #print("Setup web page requested")
      else:
         response = ""
         status_code = HTTPStatus.NOT_FOUND
         content_type = "text/html"
         #print("page not found")

      self._reply(status_code, content_type, response)

   def do_POST(self):
      log.debug("received client POST request from %s" % (self.address_string()))

      if self.path.strip("/") != GUIURL:
         self._reply(HTTPStatus.NOT_FOUND, "text/html", "")
         return

      # Only while a token is actually wanted. GUIURL is "", so this used to
      # accept a token posted to "/" at ANY time, unauthenticated - including
      # while a perfectly good session was running. Anyone on the network
      # could replace the credential: stopping the monitor, or worse,
      # pointing it at a different CareLink account so the displays showed
      # somebody else's glucose with nothing on screen to say so.
      if g_status != STATUS_NEED_TKN:
         log.warning("refused token POST from %s: not waiting for a token (%s)"
                     % (self.address_string(), g_status))
         self._reply(HTTPStatus.FORBIDDEN, "text/plain",
                     "A token can only be submitted while the proxy is waiting for one.\n")
         return

      if not self._same_origin():
         log.warning("refused cross-site token POST from %s (Origin %s)"
                     % (self.address_string(), self.headers.get("Origin")))
         self._reply(HTTPStatus.FORBIDDEN, "text/plain",
                     "Cross-site token submission refused.\n")
         return

      try:
         content_length = int(self.headers.get("Content-Length", ""))
      except ValueError:
         self._reply(HTTPStatus.LENGTH_REQUIRED, "text/plain", "Content-Length required.\n")
         return
      if content_length < 0 or content_length > MAX_POST_BYTES:
         self._reply(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "text/plain", "Request too large.\n")
         return
      body = self.rfile.read(content_length)

      try:
         qs = body.decode()
         tokendata = parse_qs(qs)['tokendata'][0]
         save_params(tokendata)
         time.sleep(2)
         response = webgui(status=g_status)
      except Exception as e:
         log.error("ERROR: failed to save posted token data: %s" % e)
         # Escaped: this text is echoed straight into the page.
         response = webgui(status=g_status, action=GUIURL, error=html.escape(str(e)))
      self._reply(HTTPStatus.OK, "text/html", response)

#################################################
# Web server thread
#################################################
def webserver_thread():
   # Init web server
   webserver = ThreadingHTTPServer((HOSTNAME, PORT), MyServer)
   log.debug("HTTP server started at http://%s:%s" % (HOSTNAME, PORT))

   # Start server loop
   webserver.serve_forever()


#################################################
# Start web server as asynchronous thread
#################################################
def start_webserver():
   t = threading.Thread(target=webserver_thread, args=())
   t.daemon = True
   t.start()


# Parse command line 
parser = argparse.ArgumentParser()
parser.add_argument('--tokenfile','-t', type=str, help='File containing auth tokens (default: %s)' % TOKENFILE, required=False)
parser.add_argument('--wait',     '-w', type=int, help='Wait seconds between repeated calls (default 300)', required=False)
parser.add_argument('--verbose',  '-v', help='Verbose mode', action='store_true')
parser.add_argument('--fallback-pskfile', type=str, help='Password file of the display fallback hotspot, served at /%s/%s (default: %s)' % (APIURL, OPT_FALLBACK, FALLBACK_PSKFILE), required=False)
args = parser.parse_args()

# Get parameters from CLI
tokenfile = TOKENFILE if args.tokenfile == None else args.tokenfile
wait      = UPDATE_INTERVAL if args.wait == None else args.wait
verbose   = args.verbose
fallback_pskfile = FALLBACK_PSKFILE if args.fallback_pskfile == None else args.fallback_pskfile

# Logging config (verbose)
if verbose:
   log.getLogger().setLevel(log.DEBUG)
   log.debug("Logging is vebose")

log.info("Starting Carelink Client Proxy (version %s)" % VERSION)

# Init signal handler
signal.signal(signal.SIGTERM, on_sigterm)
signal.signal(signal.SIGINT, on_sigterm)

# Start web server
start_webserver()

# Main process loop
init_retry_s = INIT_RETRY_FIRST_S
while True:
   # Init Carelink client
   client = carelink_client2.CareLinkClient(tokenFile=tokenfile)
   g_status = STATUS_DO_LOGIN
   
   # Login to Carelink server
   if client.init():
      g_status = STATUS_LOGIN_OK
      g_detail = ""
      init_retry_s = INIT_RETRY_FIRST_S

      # Infinite loop requesting Carelink data periodically
      i = 0
      while True:
         i += 1
         log.debug("Starting download %d" % i)

         try:
            recentData = client.getRecentData()
            if recentData != None and client.getLastResponseCode() == HTTPStatus.OK:
               log.debug("New data received")
               g_status = STATUS_LOGIN_OK
               g_last_data = time.time()
               g_detail = "Last data from CareLink: %s" % since(g_last_data)
            elif client.getLastResponseCode() == HTTPStatus.FORBIDDEN or client.getLastResponseCode() == HTTPStatus.UNAUTHORIZED:
               # Authorization error occured
               log.error("ERROR: failed to get data (Authotization error, response code %d)" % client.getLastResponseCode())
               recentData = None
               g_detail = "No data since %s: CareLink refused the token (HTTP %d)." % (since(g_last_data), client.getLastResponseCode())
               break
            else:
               # Connection error occured
               log.error("ERROR: failed to get data (Connection error, response code %s)" % client.getLastResponseCode())
               recentData = None
               g_status = STATUS_NO_DATA
               g_detail = "No data since %s: CareLink answered HTTP %s; retrying every minute." % (since(g_last_data), client.getLastResponseCode())
               time.sleep(60)
               continue
         except carelink_client2.TokenRefreshRefused as e:
            # The session is over; asking again every minute will not revive
            # it. Fall through to "Valid token required", which shows the
            # recovery form and still retries the stored token now and then.
            log.error(e)
            recentData = None
            g_detail = ("No data since %s: CareLink refused the token refresh (HTTP %d). "
                        "A new sign-in is needed. A sign-in with the same CareLink account "
                        "on another device ends this session." % (since(g_last_data), e.status_code))
            break
         except Exception as e:
            log.error(e)
            recentData = None
            g_status = STATUS_NO_DATA
            if isinstance(e, carelink_client2.requests.exceptions.RequestException):
               reason = "cannot reach CareLink (%s)" % type(e).__name__
            else:
               # Includes a refresh answered with a code not taken as a
               # refusal: say what happened rather than guess.
               reason = str(e)
            g_detail = "No data since %s: %s; retrying every minute." % (since(g_last_data), reason)
            time.sleep(60)
            continue
            
         # Calculate time until next reading
         try:
            nextReading = int(recentData["patientData"]["lastConduitUpdateServerDateTime"]/1000) + wait
            tmoSeconds  = int(nextReading - time.time())
            #print("Next reading at {0}, {1} seconds from now\n".format(nextReading,tmoSeconds))
            log.debug("Next reading at {0}, {1} seconds from now\n".format(nextReading,tmoSeconds))
            if tmoSeconds < 0:
               tmoSeconds = RETRY_INTERVAL
            # Never further ahead than one cycle. nextReading is server time
            # plus one interval, so anything longer means the local clock is
            # wrong - a Pi has no RTC and starts with the time it was switched
            # off until NTP catches up. carelink-01, off for a night, fetched
            # 18 s before the clock jumped 9 h forward and then slept 9 h on
            # fresh data (2026-10-31).
            elif tmoSeconds > wait:
               log.warning("Next reading %d s away exceeds the %d s interval; clock was off, using %d s"
                           % (tmoSeconds, wait, wait))
               tmoSeconds = wait
         except KeyError:
            tmoSeconds = RETRY_INTERVAL
            #print("Retry reading {0} seconds from now\n".format(tmoSeconds))

         log.debug("Waiting " + str(tmoSeconds) + " seconds before next download")
         time.sleep(tmoSeconds+10)

   # Wait for a new token - but not only for one. A failed start is not
   # always a bad token: after a power cut the Pi is often back before the
   # router, init() cannot even resolve the CareLink host, and waiting for a
   # POST alone left a working token unused until someone restarted the
   # service by hand (seen on carelink-01, 2026-10-24: 14 hours stuck on a
   # token that refreshed fine the moment the proxy was restarted). So keep
   # accepting a new token, and also retry the stored one, backing off to
   # INIT_RETRY_MAX_S so a token that really is dead costs one request per
   # half hour.
   recentData = None
   if client.initFailedTransient():
      # Not a token problem: CareLink could not be reached. Keep the short
      # retry so data is back within a minute of the network.
      g_status = STATUS_NO_DATA
      g_detail = "No data since %s: cannot reach CareLink; retrying every minute." % since(g_last_data)
      retry_s = INIT_RETRY_FIRST_S
      log.info("CareLink not reachable; retrying in %d s" % retry_s)
   else:
      log.info(STATUS_NEED_TKN)
      g_status = STATUS_NEED_TKN
      # Keep the more specific reason a refused refresh already gave.
      if "refused the token" not in g_detail:
         g_detail = "No data since %s: CareLink did not accept the stored token." % since(g_last_data)
      retry_s = init_retry_s
      init_retry_s = min(init_retry_s * 2, INIT_RETRY_MAX_S)
      log.info("Retrying with the stored token in %d s" % retry_s)
   # Monotonic: a Pi without an RTC jumps its wall clock when NTP arrives,
   # which made this wait end early or late.
   retry_at = time.monotonic() + retry_s
   wait_for_params = True
   while wait_for_params and time.monotonic() < retry_at:
      time.sleep(0.1)

# Exit         
log.info("Exit")
