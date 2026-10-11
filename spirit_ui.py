#!/usr/bin/env python3
"""spirit_ui.py - a local web page for the Spiritual Successor tool.

One Python file, standard library only. Linux, no root, no dependencies - same promise the
tool itself makes. It shells out to `spirit.py` (read-only commands on page load, the slow
ones only when you press a button) and shows the result as a page.

Run it:      python3 spirit_ui.py
Flags:       --port N     listen on this port instead of a free one
             --no-open    do not open a browser
             --print-url  start, print the URL, and keep running

The page is served on 127.0.0.1 only, under a token in its path. Why the token exists is
explained next to TOKEN below.
"""

import argparse
import html
import json
import os
import re
import secrets
import shlex
import socket
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SPIRIT = os.path.join(BASE_DIR, "spirit.py")
PY = sys.executable or "python3"

# ---------------------------------------------------------------------------------------------
# CSRF guard.
#
# A page you merely have open in another tab can POST to http://127.0.0.1:<port>/ all day. It
# cannot read a response, but it does not need to - a fire-and-forget form post is enough to
# make this machine launch a game. So every request has to carry a random token in its PATH:
# a cross-site form or fetch cannot know the token, and same-origin script cannot read another
# origin's URL. No token, no service - 403. This is the whole reason the URL has /<token>/ in it.
# ---------------------------------------------------------------------------------------------
TOKEN = secrets.token_urlsafe(16)

# One running (or finished) action, buffered so the page can poll it without blocking.
JOBS = {}
JOBS_LOCK = threading.Lock()
PROCS = []
PROCS_LOCK = threading.Lock()


# ---------------------------------------------------------------------------------------------
# talking to spirit.py
# ---------------------------------------------------------------------------------------------
def _run(args, timeout=30):
    """Run a fast, read-only spirit command and return (rc, text). Never raises."""
    try:
        r = subprocess.run([PY, SPIRIT] + args, capture_output=True, text=True,
                           timeout=timeout, cwd=BASE_DIR)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except Exception as exc:
        return 1, "could not run %s: %s" % (" ".join(args), exc)


def _json(args, timeout=30):
    rc, text = _run(args + ["--json"], timeout=timeout)
    try:
        return json.loads(text)
    except Exception:
        return None


def _parse_boost(text):
    """Pull the facts the page needs out of the boost report. Anything missing stays None."""
    out = {"native": None, "upscale": (None, ""), "framegen": (None, ""), "witness": (None, "")}
    m = re.search(r"display\s*:\s*(\d+)x(\d+)", text)
    if m:
        out["native"] = (int(m.group(1)), int(m.group(2)))
    for line in text.splitlines():
        low = line.strip()
        m = re.match(r"gamescope\s*:\s*(\S+)\s+(.*)", low)
        if m:
            out["upscale"] = (m.group(1), m.group(2).strip())
        m = re.match(r"lsfg-vk\s*:\s*(\S+)\s+(.*)", low)
        if m:
            out["framegen"] = (m.group(1), m.group(2).strip())
        m = re.match(r"witness\s*:\s*(\S+)\s+(.*)", low)
        if m:
            out["witness"] = (m.group(1), m.group(2).strip())
    return out


def _render_res(native, factor):
    """Same even-rounded render size the tool itself computes."""
    w = max(2, int(round(native[0] * factor))) // 2 * 2
    h = max(2, int(round(native[1] * factor))) // 2 * 2
    return w, h


def _short(name, limit=46):
    n = (name or "").strip()
    return n if len(n) <= limit else n[:limit - 1] + "\u2026"


# ---------------------------------------------------------------------------------------------
# background actions: doctor and play. A thread per action writes into a buffer; the page polls.
# ---------------------------------------------------------------------------------------------
def _new_job():
    jid = secrets.token_hex(8)
    with JOBS_LOCK:
        JOBS[jid] = {"output": "", "done": False, "rc": None}
    return jid


def _job_append(jid, text):
    with JOBS_LOCK:
        JOBS[jid]["output"] += text


def _run_job(jid, argv):
    try:
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, bufsize=1, cwd=BASE_DIR)
    except Exception as exc:
        _job_append(jid, "could not start: %s\n" % exc)
        with JOBS_LOCK:
            JOBS[jid]["done"], JOBS[jid]["rc"] = True, -1
        return
    with PROCS_LOCK:
        PROCS.append(p)
    try:
        for line in p.stdout:
            _job_append(jid, line)
    except Exception:
        pass
    finally:
        try:
            rc = p.wait()
        except Exception:
            rc = -1
        with PROCS_LOCK:
            if p in PROCS:
                PROCS.remove(p)
        with JOBS_LOCK:
            JOBS[jid]["done"], JOBS[jid]["rc"] = True, rc


def start_doctor():
    jid = _new_job()
    _job_append(jid, "Running spirit doctor - this launches a real program on every card, "
                     "so it takes about a minute.\n\n")
    threading.Thread(target=_run_job, args=(jid, [PY, SPIRIT, "doctor"]), daemon=True).start()
    return jid


def start_play(cmd, scale, framegen):
    argv = [PY, SPIRIT, "play"]
    if scale:
        argv += ["--scale", scale]
    if framegen:
        argv += ["--framegen", str(framegen)]
    argv += ["--"] + cmd
    jid = _new_job()
    threading.Thread(target=_run_job, args=(jid, argv), daemon=True).start()
    return jid


# ---------------------------------------------------------------------------------------------
# the page
# ---------------------------------------------------------------------------------------------
CSS = """
:root { color-scheme: light dark; }
* { box-sizing: border-box; }
body { margin: 0; font: 15px/1.55 system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
       background: #f6f5f2; color: #23211e; }
main { max-width: 860px; margin: 0 auto; padding: 32px 20px 80px; }
h1 { font-size: 24px; margin: 0 0 4px; letter-spacing: -0.01em; }
.sub { color: #6b675f; margin: 0 0 30px; }
section { background: #fff; border: 1px solid #e3e0d9; border-radius: 10px;
          padding: 20px 22px; margin: 0 0 18px; }
h2 { font-size: 17px; margin: 0 0 14px; }
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; padding: 9px 10px; border-bottom: 1px solid #eeecE6; }
th { font-size: 12px; text-transform: uppercase; letter-spacing: 0.04em; color: #6b675f; }
tr:last-child td { border-bottom: none; }
.mine { color: #1d6f42; font-weight: 600; }
.spare { color: #6b675f; }
.row { padding: 12px 0; border-bottom: 1px solid #f0eee9; }
.row:last-child { border-bottom: none; }
.row h3 { font-size: 15px; margin: 0 0 6px; }
.state { font-weight: 600; }
.state.ok { color: #1d6f42; }
.state.no { color: #9a3b2e; }
.why { color: #6b675f; margin: 6px 0 0; }
label.opt { display: inline-block; margin: 8px 14px 0 0; }
select, input[type=text], textarea { font: inherit; padding: 7px 9px; border: 1px solid #cfcbc2;
       border-radius: 7px; background: #fff; color: inherit; }
textarea { width: 100%; min-height: 62px; resize: vertical; font-family: ui-monospace, monospace;
       font-size: 13px; }
button { font: inherit; padding: 8px 16px; border: 1px solid #23211e; border-radius: 7px;
         background: #23211e; color: #fff; cursor: pointer; }
button:disabled { opacity: 0.5; cursor: default; }
pre { background: #1c1b19; color: #e8e4dc; padding: 14px 16px; border-radius: 8px; overflow-x: auto;
      font: 12.5px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace; white-space: pre-wrap;
      word-break: break-word; }
.hint { color: #6b675f; font-size: 13.5px; margin: 0 0 12px; }
.note { color: #6b675f; font-size: 13.5px; margin: 10px 0 0; }
.hidden { display: none; }
.honest { color: #6b675f; }
"""

JS = """
function statusUrl(id) { return "/__TOKEN__/api/job?id=" + encodeURIComponent(id); }
function poll(jid, pre, btn) {
  function tick() {
    fetch(statusUrl(jid)).then(function (r) { return r.json(); }).then(function (d) {
      pre.textContent = d.output;
      pre.scrollTop = pre.scrollHeight;
      if (!d.done) { setTimeout(tick, 800); }
      else if (btn) { btn.disabled = false; }
    }).catch(function () { setTimeout(tick, 1200); });
  }
  tick();
}
function showScale() {
  var sel = document.getElementById("scale");
  var out = document.getElementById("scaleres");
  var v = sel.value;
  if (!v) { out.textContent = "render at full " + NATIVE[0] + "x" + NATIVE[1]; return; }
  var r = SCALES[v];
  out.textContent = "render " + r.w + "x" + r.h + ", " + r.pct + "% of the pixels";
}
function launch() {
  var btn = document.getElementById("playbtn");
  var cmd = document.getElementById("cmd").value.trim();
  var pre = document.getElementById("playout");
  if (!cmd) { pre.textContent = "Type your game command first."; return; }
  btn.disabled = true;
  pre.textContent = "Starting\\u2026";
  var body = "cmd=" + encodeURIComponent(cmd) +
             "&scale=" + encodeURIComponent(document.getElementById("scale").value) +
             "&framegen=" + encodeURIComponent(document.getElementById("framegen").value);
  fetch("/__TOKEN__/api/play", { method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: body })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d.error) { pre.textContent = d.error; btn.disabled = false; return; }
      poll(d.job, pre, btn);
    }).catch(function (e) { pre.textContent = "Could not start: " + e; btn.disabled = false; });
}
function doctor() {
  var btn = document.getElementById("docbtn");
  var pre = document.getElementById("docout");
  btn.disabled = true;
  pre.classList.remove("hidden");
  pre.textContent = "Running\\u2026";
  fetch("/__TOKEN__/api/doctor", { method: "POST" })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d.error) { pre.textContent = d.error; btn.disabled = false; return; }
      poll(d.job, pre, btn);
    }).catch(function (e) { pre.textContent = "Could not start: " + e; btn.disabled = false; });
}
"""

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Spiritual Successor</title>
<style>__CSS__</style></head>
<body><main>
<h1>Spiritual Successor</h1>
<p class="sub">Your game on one card. The other cards do the jobs beside it.</p>

<section>
  <h2>Your cards</h2>
  <table><thead><tr><th>Card</th><th>Link (estimated)</th><th>Doing</th></tr></thead>
  <tbody>__CARDS__</tbody></table>
</section>

<section>
  <h2>More frames</h2>
  <div class="row">
    <h3>Upscaling &mdash; render below native, scale back up</h3>
    __UPSCALE__
    <label class="opt">Render scale:
      <select id="scale" onchange="showScale()">__SCALEOPTS__</select>
    </label>
    <p class="note" id="scaleres"></p>
  </div>
  <div class="row">
    <h3>Frame generation &mdash; extra frames, interpolated</h3>
    __FRAMEGEN__
    <label class="opt">Multiplier:
      <select id="framegen">__FGOPTS__</select>
    </label>
    <p class="note">__WITNESS__</p>
  </div>
</section>

<section>
  <h2>Play</h2>
  <p class="hint">Paste the command that starts your game. Pick a scale or multiplier above if
  you want them. The game launches on this machine while the page shows what is happening.</p>
  <textarea id="cmd" placeholder="./yourgame"></textarea>
  <p><button id="playbtn" onclick="launch()">Launch</button></p>
  <pre id="playout" class="hidden"></pre>
</section>

<section>
  <h2>Check my machine</h2>
  <p class="hint">Tests whether the tool really works here. It launches a real program on every
  card, so give it about a minute.</p>
  <p><button id="docbtn" onclick="doctor()">Run doctor</button></p>
  <pre id="docout" class="hidden"></pre>
</section>

<p class="note honest">Linux. No root. No packages to install. Everything on this page is read from
your own machine.</p>
</main>
<script>
var NATIVE = __NATIVE__;
var SCALES = __SCALES__;
__JS__
showScale();
</script>
</body></html>
"""


def _state_html(state, detail, ok_when):
    cls = "ok" if state == ok_when else "no"
    return ('<p class="state %s">%s</p>' % (cls, html.escape(state or "UNKNOWN")))


def render_page():
    probe = _json(["probe"]) or {}
    roles = _json(["roles"]) or []
    rc, boost_text = _run(["boost"], timeout=30)
    boost = _parse_boost(boost_text)

    gpus = {g.get("card"): g for g in (probe.get("gpus") or [])}
    role_by_card = {r.get("card"): r for r in roles}

    # --- cards table -------------------------------------------------------------------
    rows = []
    order = [r.get("card") for r in roles] or list(gpus.keys())
    for card in order:
        r = role_by_card.get(card, {})
        g = gpus.get(card, {})
        name = _short(r.get("name") or g.get("name") or card)
        link = g.get("link") or {}
        if link:
            link_txt = "x%s %s, %s GB/s max" % (link.get("max_width"), link.get("max_gen"),
                                                link.get("gbps_max"))
        else:
            link_txt = "%s GB/s max" % r.get("link_gbps_max")
        is_main = "display" in (r.get("roles") or [])
        doing = ('<span class="mine">your game</span>' if is_main
                 else '<span class="spare">spare &mdash; available for jobs</span>')
        rows.append("<tr><td>%s</td><td>%s</td><td>%s</td></tr>"
                    % (html.escape(name), html.escape(link_txt), doing))
    cards_html = "\n".join(rows) or '<tr><td colspan="3">No cards found.</td></tr>'

    # --- upscaling ---------------------------------------------------------------------
    us_state, us_detail = boost["upscale"]
    native = boost["native"]
    if us_state == "READY":
        up_html = ('<p class="state ok">Upscaling is available on this machine.</p>'
                   '<p class="why">%s</p>' % html.escape(us_detail))
    else:
        up_html = ('<p class="state no">Upscaling is not available here.</p>'
                   '<p class="why">%s</p>' % html.escape(us_detail or "gamescope was not found."))

    scale_opts = ['<option value="">off</option>']
    scales_json = {}
    if native:
        for f in ("0.9", "0.8", "0.7", "0.6"):
            rw, rh = _render_res(native, float(f))
            pct = round(100.0 * float(f) * float(f))
            scales_json[f] = {"w": rw, "h": rh, "pct": pct}
            scale_opts.append('<option value="%s">%s &rarr; %dx%d (%d%%)</option>'
                              % (f, f, rw, rh, pct))
    else:
        scale_opts.append('<option value="" disabled>no display resolution could be read</option>')

    # --- frame generation --------------------------------------------------------------
    fg_state, fg_detail = boost["framegen"]
    w_state, w_detail = boost["witness"]
    if fg_state == "INSTALLED":
        fg_html = ('<p class="state ok">Frame generation is available on this machine.</p>'
                   '<p class="why">%s</p>' % html.escape(fg_detail))
    else:
        fg_html = ('<p class="state no">Frame generation is not available here.</p>'
                   '<p class="why">%s</p>'
                   % html.escape(fg_detail or "lsfg-vk was not found."))
    fg_opts = ['<option value="">off</option>'] + \
              ['<option value="%d">%dx</option>' % (n, n) for n in (2, 3, 4)]

    if w_state == "READY":
        witness_html = ("The result can be measured here: the present counter is installed, so a "
                        "frame-gen run is checked, not asserted.")
    else:
        witness_html = ("The present counter is missing, so a frame-gen claim cannot be measured "
                        "on this machine. %s" % html.escape(w_detail or ""))

    page = (PAGE
            .replace("__CSS__", CSS)
            .replace("__JS__", JS)
            .replace("__TOKEN__", TOKEN)
            .replace("__CARDS__", cards_html)
            .replace("__UPSCALE__", up_html)
            .replace("__FRAMEGEN__", fg_html)
            .replace("__WITNESS__", witness_html)
            .replace("__SCALEOPTS__", "".join(scale_opts))
            .replace("__FGOPTS__", "".join(fg_opts))
            .replace("__NATIVE__", json.dumps(list(native) if native else [0, 0]))
            .replace("__SCALES__", json.dumps(scales_json)))
    return page


# ---------------------------------------------------------------------------------------------
# the server
# ---------------------------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    server_version = "spirit-ui/1.0.0"

    def log_message(self, *a):  # quiet by default; the page is the interface
        pass

    def _segments(self):
        path = urlparse(self.path).path
        return [s for s in path.split("/") if s]

    def _token_ok(self, seg):
        return bool(seg) and seg[0] == TOKEN

    def _forbidden(self):
        body = b"No token. This page only answers under its own URL, with the token in the path.\n"
        self.send_response(403)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, text):
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        seg = self._segments()
        if not self._token_ok(seg):
            return self._forbidden()
        rest = seg[1:]
        if rest == ["api", "job"]:
            qid = (parse_qs(urlparse(self.path).query).get("id") or [""])[0]
            with JOBS_LOCK:
                job = JOBS.get(qid)
            if not job:
                return self._send_json({"error": "unknown job"})
            return self._send_json(dict(job))
        if rest == []:
            return self._send_html(render_page())
        self.send_error(404)

    def do_POST(self):
        seg = self._segments()
        if not self._token_ok(seg):
            return self._forbidden()
        rest = seg[1:]
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        form = parse_qs(raw)
        if rest == ["api", "doctor"]:
            return self._send_json({"job": start_doctor()})
        if rest == ["api", "play"]:
            cmd_text = (form.get("cmd") or [""])[0].strip()
            scale = (form.get("scale") or [""])[0].strip()
            framegen = (form.get("framegen") or [""])[0].strip()
            if not cmd_text:
                return self._send_json({"error": "Type your game command first."})
            if scale not in ("", "0.9", "0.8", "0.7", "0.6"):
                return self._send_json({"error": "Unknown render scale."})
            if framegen not in ("", "2", "3", "4"):
                return self._send_json({"error": "Unknown frame-gen multiplier."})
            try:
                cmd = shlex.split(cmd_text)
            except ValueError as exc:
                return self._send_json({"error": "Could not read that command: %s" % exc})
            return self._send_json({"job": start_play(cmd, scale, int(framegen) if framegen else None)})
        self.send_error(404)


def main():
    ap = argparse.ArgumentParser(description="A local web page for the Spiritual Successor tool.")
    ap.add_argument("--port", type=int, default=0, help="port to listen on (default: a free one)")
    ap.add_argument("--no-open", action="store_true", help="do not open a browser")
    ap.add_argument("--print-url", action="store_true", help="print the URL and keep running")
    args = ap.parse_args()

    if not os.path.isfile(SPIRIT):
        print("spirit.py is not beside this file (%s). Nothing to drive." % SPIRIT, file=sys.stderr)
        return 1

    # Bind to 127.0.0.1 only. If we cannot, we stop rather than quietly listen on the network.
    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError as exc:
        print("Could not listen on 127.0.0.1:%s (%s). Refusing to serve anywhere else."
              % (args.port, exc), file=sys.stderr)
        return 1
    port = httpd.server_address[1]
    url = "http://127.0.0.1:%d/%s/" % (port, TOKEN)

    print(url, flush=True)
    if not args.no_open:
        try:
            webbrowser.open(url)
        except Exception:
            pass

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        httpd.shutdown()
        httpd.server_close()
        # No orphaned children: stop anything a Doctor or Play run left behind.
        with PROCS_LOCK:
            for p in list(PROCS):
                try:
                    p.terminate()
                except Exception:
                    pass
            for p in list(PROCS):
                try:
                    p.wait(timeout=5)
                except Exception:
                    try:
                        p.kill()
                    except Exception:
                        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
