#!/usr/bin/env python3
"""spirit — the Spiritual Successor: put EVERY GPU in a machine to work.

CrossFire and SLI split one frame across cards and died for it. The Spiritual Successor does the
opposite: it splits the WORK, not the frame, and gives each card the role its own link and silicon
are actually good for — so a cheap second, third, or tenth GPU earns its slot instead of sitting
idle. Scale is the point, not speed: the goal is that every card in the box is doing something
useful by the time the game is running.

No root. No dependencies (stdlib Python 3.8+; `lspci` used only if present).

THE TWO TRUTHS THIS IS BUILT ON (both measured, see the project folder):
  1. Every normal surface lies about a GPU's host link. lspci / sysfs current_link_width /
     pp_dpm_pcie all report the link to the GPU's OWN bridge chip, not to the CPU. The truth is the
     MINIMUM negotiated width along the sysfs chain from the root complex down. This probe reads it.
  2. Per-frame multi-GPU rendering is DEAD on consumer links (a 1440p frame is 14.7 MB; a Gen3 x1
     card needs 17 ms to move it, more than the frame budget). So: never split the frame. Split the
     process and the pipeline tail; cross the wire at frame rate or once.

Commands:
    spirit probe [--json]                      every GPU: true link, VRAM, outputs, render node
    spirit roles [--profile game|edit|compute] assign a role to each card so all are used
    spirit plan  --workload 1440p60-framegen   honest feasibility of a workload per card
    spirit run   --role encode --card card2 -- ffmpeg ...

Copyright 2026 Sophia Marie DeClue. MIT.
"""
import os, sys, json, glob, re, shutil, subprocess, argparse, tempfile, time

SPEED_GBPS = {2.5: 0.25, 5.0: 0.50, 8.0: 1.00, 16.0: 1.97, 32.0: 4.00}   # GB/s per lane, theoretical
GEN_NAME = {2.5: "Gen1", 5.0: "Gen2", 8.0: "Gen3", 16.0: "Gen4", 32.0: "Gen5"}
EFF = 0.85             # measured efficiency of the theoretical ceiling on this class of box
PLAN_HEADROOM = 0.60   # use at most 60% of a link
CARD_RE = re.compile(r"^card\d+$")


def _read(p):
    try:
        with open(p) as fh:
            return fh.read().strip()
    except OSError:
        return None


def _int(p):
    v = _read(p)
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _speed(p):
    v = _read(p)
    if not v:
        return None
    try:
        return float(v.split()[0])
    except (IndexError, ValueError):
        return None


# ── the true host link: minimum negotiated width/speed along the sysfs chain ──────────
def chain_for(bdf):
    real = os.path.realpath("/sys/bus/pci/devices/%s" % bdf)
    segs = [s for s in real.split("/") if s.startswith("0000:")]
    return segs or [bdf]


def binding_link(bdf):
    """(binding_element, min_current_w, min_current_s, min_max_w, min_max_s).

    Currents and maxima are each taken independently. Size and sort on the MAXIMA:
    `current_link_*` DROPS AT IDLE, so a table sorted on current links reorders itself run to run.
    """
    mw = mseg = ms = xw = xs = None
    for seg in chain_for(bdf):
        base = "/sys/bus/pci/devices/%s" % seg
        w = _int(base + "/current_link_width")
        if w is not None and (mw is None or w < mw):
            mw, mseg = w, seg
        s = _speed(base + "/current_link_speed")
        if s is not None and (ms is None or s < ms):
            ms = s
        xwi = _int(base + "/max_link_width")
        if xwi is not None and (xw is None or xwi < xw):
            xw = xwi
        xsi = _speed(base + "/max_link_speed")
        if xsi is not None and (xs is None or xsi < xs):
            xs = xsi
    return (mseg, mw, ms, xw, xs)


def link_gbps(width, speed):
    if not width or not speed:
        return None
    per = SPEED_GBPS.get(speed)
    return round(per * width * EFF, 2) if per else None


def _lspci_name(bdf):
    if not shutil.which("lspci"):
        return None
    try:
        out = subprocess.run(["lspci", "-mm", "-s", bdf], capture_output=True, text=True, timeout=5).stdout
        if '"' in out:
            parts = out.split('"')
            if len(parts) >= 8:
                return ("%s %s" % (parts[5], parts[7])).strip()
    except Exception:
        pass
    return None


def connectors(card):
    conn = glob.glob("/sys/class/drm/%s-*/status" % card)
    online = sum(1 for c in conn if _read(c) == "connected")
    return online, len(conn)


def _render_node(card):
    g = glob.glob("/sys/class/drm/%s/device/drm/renderD*" % card)
    return os.path.basename(g[0]) if g else None


# ── probe: enumerate every GPU, fleet-scale (N cards, any vendor mix) ──────────────────
def probe():
    fake = os.environ.get("SPIRIT_DEVICES")
    if fake and os.path.exists(fake):
        db = json.load(open(fake))
        db.setdefault("gpufan", "probe/0.1")
        return db
    gpus = []
    cards = [c for c in glob.glob("/sys/class/drm/card*")
             if CARD_RE.match(os.path.basename(c))]
    for card in sorted(cards, key=lambda c: int(os.path.basename(c)[4:])):
        base = os.path.join(card, "device")
        if not os.path.exists(base):
            continue
        bdf = os.path.basename(os.path.realpath(base))
        vendor = _read(base + "/vendor") or ""
        device = _read(base + "/device") or ""
        uev = _read(base + "/uevent") or ""
        driver = next((l.split("=", 1)[1] for l in uev.splitlines() if l.startswith("DRIVER=")), "")
        mseg, mw, ms, xw, xs = binding_link(bdf)
        own_w = _int(base + "/current_link_width")
        total = _int(base + "/mem_info_vram_total")
        used = _int(base + "/mem_info_vram_used")
        cn = os.path.basename(card)
        online, nconn = connectors(cn)
        integrated = total is not None and total < 2 * 1024 ** 3
        gpus.append({
            "card": cn, "bdf": bdf,
            "name": _lspci_name(bdf) or ("%s:%s" % (vendor[2:], device[2:])),
            "driver": driver, "vendor": vendor, "device": device,
            "integrated": integrated,
            "render_node": _render_node(cn),
            "vram_total_mib": round(total / 1048576) if total else None,
            "vram_used_mib": round(used / 1048576) if used else None,
            "display_outputs": online, "connectors_total": nconn,
            "has_output": online > 0,
            "link": {
                "binding_element": mseg,
                "max_width": xw, "max_gen": GEN_NAME.get(xs, ("%.1f GT/s" % xs) if xs else None),
                "gbps_max": link_gbps(xw, xs),
                "current_width": mw, "current_gen": GEN_NAME.get(ms, ("%.1f GT/s" % ms) if ms else None),
                "gbps_current": link_gbps(mw, ms),
                "idle_degraded": (mw is not None and xw is not None and mw < xw)
                                 or (ms is not None and xs is not None and ms < xs),
                "own_reports_width": own_w,
                "capped_by_topology": (own_w is not None and xw is not None and own_w > xw),
            },
        })
    gpus.sort(key=lambda g: -(g["link"]["gbps_max"] or 0))
    return {"gpufan": "probe/0.1", "gpus": gpus}


def score(g):
    """Ordering only (never a claim of speed): link capability + VRAM, integrated deprioritised."""
    base = (g["link"]["gbps_max"] or 0) + (g.get("vram_total_mib") or 0) / 4096.0
    return base - 5 if g.get("integrated") else base


# ── roles: the heart of the Spiritual Successor — no card is left idle without a reason ──
def assign_roles(db, profile="game"):
    """Give every card the job it is best suited to. Nothing is left idle without a stated reason."""
    g = list(db["gpus"])
    ranked = sorted(g, key=lambda x: -score(x))
    used, reasons = {}, {}

    def add(c, role, why):
        used.setdefault(c["card"], []).append(role)
        reasons.setdefault(c["card"], []).append(why)

    def pick(role, pred, why, exclude=()):
        for c in ranked:
            if c["card"] in exclude or role in used.get(c["card"], []):
                continue
            if pred(c):
                add(c, role, why)
                return c
        return None

    # 1) render — ALWAYS the card feeding the monitor. The user plugs the panel into the card they
    #    mean to game on, which is almost always the strongest; and rendering on the presenter avoids
    #    a cross-GPU present, which is a per-frame PIXEL copy -- the exact thing measured dead.
    monitor_cards = [c for c in ranked if c["has_output"]]
    render = max(monitor_cards, key=score) if monitor_cards else (ranked[0] if ranked else None)
    rd = render["card"] if render else None
    if render:
        add(render, "render",
            "FEEDS THE MONITOR — the main GPU by rule (rendering anywhere else costs a per-frame "
            "cross-GPU present)" if render["has_output"] else
            "strongest card by score (NO monitor detected — headless rig)")

    # 2) display — the render card owns the panel it presents to (render == display, on purpose)
    display = render
    dd = rd
    if display:
        add(display, "display", "owns the connected panel" if display["has_output"]
            else "assumed display (no panel detected)")

    # 3) frame-gen + upscale — a SECOND card that owns a panel; one-way frame-rate traffic only
    fg = next((c for c in ranked if c["has_output"] and c["card"] != dd), None)
    if fg:
        add(fg, "framegen", "second panel owner: interpolated frames are generated on the display GPU")
        add(fg, "upscale", "same card spatial-upscales the presented frame")
    else:
        cand = next((c for c in ranked if c.get("connectors_total") and c["card"] != dd), None)
        if cand:
            reasons.setdefault(cand["card"], []).append(
                "SPARE framegen/upscale target — plug a display into one of its %d connectors to activate"
                % cand["connectors_total"])

    # 4) encode — best amdgpu that is NOT the renderer (AMD VCN/VCE needs no display output)
    pick("encode", lambda c: c["driver"] == "amdgpu" and c["card"] != rd,
         "VCN/VCE video encode (AMD has no concurrent-session limit) — records/streams off the render card")

    # 5) compute — the largest VRAM OFF the render card (agents / LLM / Blender tiles cross once)
    nonrender = [c for c in ranked if c["card"] != rd]
    comp = max(nonrender, key=lambda c: (c.get("vram_total_mib") or 0), default=None)
    if comp:
        add(comp, "compute", "largest VRAM off the render card: process-level compute (agents, LLM, Blender)")

    # 6) analysis — a non-render card carrying the fewest roles so far
    ana = min((c for c in ranked if c["card"] != rd),
              key=lambda c: len(used.get(c["card"], [])), default=None)
    if ana:
        add(ana, "analysis", "batch lane: video analysis, proxies, preview bakes (deadline-free)")

    # 7) desktop/compositor — an integrated GPU that owns the panel frees the gaming card's VRAM
    pick("desktop", lambda c: c.get("integrated") and c["has_output"],
         "drive the desktop/compositor so the gaming card keeps its VRAM for frames")

    # honest advice: if a stronger card exists that feeds no monitor, say so (we still obey the rule)
    if render and render["has_output"]:
        stronger = [c for c in ranked if c["card"] != rd and not c["has_output"]
                    and score(c) > score(render)]
        if stronger:
            reasons.setdefault(rd, []).append(
                "NOTE: %s scores higher but feeds no monitor — plug your display into it to make IT "
                "the main GPU (we keep the monitor card as main until you do)" % stronger[0]["name"][:26])

    # 8) compute-pool — every remaining card with usable VRAM joins the SYSTEM lane, so the box
    #    earns its slots even when a single game cannot use card #9 for frames.
    for c in ranked:
        if c["card"] in used:
            continue
        if (c.get("vram_total_mib") or 0) >= 2048:
            add(c, "compute-pool",
                "spare card: joins the system compute pool (agents, renders, transcodes) — "
                "no in-game framegraph role, but the box uses it")
        else:
            reasons.setdefault(c["card"], []).append(
                "too little VRAM for a useful role (integrated; shares system RAM). "
                "Plug the panel into the motherboard to make it the desktop card.")

    plan = []
    for c in db["gpus"]:
        roles = used.get(c["card"], [])
        plan.append({
            "card": c["card"], "name": c["name"], "bdf": c["bdf"],
            "link_gbps_max": c["link"]["gbps_max"], "vram_total_mib": c["vram_total_mib"],
            "roles": roles or ["idle"],
            "why": reasons.get(c["card"], []) or ["idle — no role on this link beats doing nothing; not faked"],
        })
    return plan


def print_roles(db, profile="game"):
    db_roles = assign_roles(db, profile)
    n = len(db_roles)
    used_cards = sum(1 for r in db_roles if r["roles"] != ["idle"])
    print("spirit roles — profile: %s — %d GPU(s) detected, %d with a job\n" % (profile, n, used_cards))
    print("%-6s %-26s %-9s %-8s %s" % ("card", "name", "link", "VRAM", "role(s)"))
    print("-" * 104)
    for r in db_roles:
        print("%-6s %-26s %-9s %-8s %s" % (
            r["card"], r["name"][:26], r["link_gbps_max"] or "-",
            ("%sM" % r["vram_total_mib"]) if r["vram_total_mib"] else "-",
            ", ".join(r["roles"])))
        for w in r["why"]:
            print("        └─ %s" % w)
    print("\nEvery card is either working or explicitly idle-with-a-reason. That is the whole point:")
    print("the Spiritual Successor uses ALL the silicon, not just the fastest card.")


# ── plan: the honest feasibility arithmetic ───────────────────────────────────────────
WORKLOADS = {
    "1440p60-framegen": dict(res=(2560, 1440), bpp=4, fps=60, live=False,
                             why="frame-gen with the display on the secondary: base frames cross once, one way"),
    "4k60-framegen":    dict(res=(3840, 2160), bpp=4, fps=60, live=False,
                             why="same at 4K; needs ~8 GB/s sustained"),
    "1440p-perframe-split": dict(res=(2560, 1440), bpp=4, fps=60, live=True,
                             why="AFR/SFR — needs the frame back EVERY frame: expect infeasible"),
    "llm-13b":          dict(tokens_per_s=40, hidden=4096, bpp=2, live=False,
                             why="process-level compute; only token/KV traffic crosses, ~KB/token"),
}


def plan(db, workload, vram_mib=None):
    w = WORKLOADS.get(workload)
    if not w:
        print("unknown workload. known: " + ", ".join(WORKLOADS)); return 1
    if "res" in w:
        S = w["res"][0] * w["res"][1] * w["bpp"]
        per_sec = S * w["fps"]
    else:
        S = w["hidden"] * w["bpp"]
        per_sec = S * w["tokens_per_s"]
    print("plan — %s\n  %s\n" % (workload, w["why"]))
    print("  payload: %.2f MB %s" % (S / 1e6, "per frame" if "res" in w else "per token"))
    print("  sustained one-way: %.2f GB/s\n" % (per_sec / 1e9))
    if vram_mib:
        print("  VRAM asked: %d MiB (measured on this box: a light load costs ~800 MiB, a GPU-bound"
              " 4K load ~8,200 MiB — ~10x)\n" % vram_mib)
    print("  %-8s %-11s %-10s %-9s %-8s %s" % ("card", "link GB/s", "capacity", "verdict", "VRAM", "arithmetic"))
    print("  " + "-" * 104)
    for g in db["gpus"]:
        B = g["link"]["gbps_max"]
        if not B:
            continue
        cap = B * PLAN_HEADROOM
        load = per_sec / 1e9
        if w.get("live"):
            verdict = "INFEASIBLE"
            arith = "live: result must return every frame (%.1f%% of link at 100%%, latency on the critical path)" % (load / B * 100)
        elif load <= cap:
            verdict = "ok" if load <= cap * 0.7 else "marginal"
            arith = "%.1f%% of link (cap %.1f%%)" % (load / B * 100, PLAN_HEADROOM * 100)
        else:
            verdict = "INFEASIBLE"
            arith = "%.1f%% of link > %.1f%% cap" % (load / B * 100, PLAN_HEADROOM * 100)
        if not g["has_output"] and "framegen" in workload:
            verdict = "INFEASIBLE"
            arith = ("no panel attached — has %d connector(s); plug a display in to use as the frame-gen target"
                     % g["connectors_total"]) if g.get("connectors_total") else \
                    "no display outputs at all — cannot be a frame-gen target"
        vram = g.get("vram_total_mib")
        vcol = ("%dM" % vram) if vram else "-"
        if vram_mib and vram:
            if vram_mib > vram:
                verdict = "INFEASIBLE"
                arith = "needs %d MiB VRAM; this card has %d MiB" % (vram_mib, vram)
            else:
                vcol = "%dM ok" % vram
        print("  %-8s %-11s %-10s %-9s %-8s %s" % (g["card"], B, "%.2f GB/s" % cap, verdict, vcol, arith))
    return 0


# ── run: route a real command to the card that should do the job ──────────────────────
def run(db, role, card, cmd):
    g = next((x for x in db["gpus"] if x["card"] == card or x["bdf"] == card), None)
    if not g:
        print("no such card: %s" % card, file=sys.stderr); return 2
    env = dict(os.environ)
    pci = "pci-%s" % g["bdf"]
    env["DRI_PRIME"] = pci
    # the trailing `!` is EXCLUSIVE: without it the layer merely REORDERS and the child still
    # enumerates every GPU. Measured 2026-10-09: `1002:6860` -> 4 devices visible, `1002:6860!` -> 1.
    env["MESA_VK_DEVICE_SELECT"] = "%s:%s!" % (g["vendor"][2:], g["device"][2:])
    if role == "compute":
        env["GGML_VK_VISIBLE_DEVICES"] = g["bdf"]
    print("# routing role=%s to %s (%s, link %s GB/s)" % (role, g["card"], g["name"][:34], g["link"]["gbps_max"]))
    print("# env: DRI_PRIME=%s  MESA_VK_DEVICE_SELECT=%s" % (pci, env["MESA_VK_DEVICE_SELECT"]))
    if role == "encode" and g.get("render_node"):
        env["SPIRIT_VAAPI_DEVICE"] = "/dev/dri/%s" % g["render_node"]
        print("#      add to ffmpeg:  -vaapi_device %s" % env["SPIRIT_VAAPI_DEVICE"])
    if role == "compute":
        print("#      ggml: GGML_VK_VISIBLE_DEVICES=%s" % g["bdf"])
    sys.stdout.flush()
    if not cmd:
        return 0
    return subprocess.call(cmd, env=env)


def print_table(db):
    print("spirit probe — the TRUE host link of each GPU (min negotiated width along the sysfs chain)\n")
    print("%-6s %-13s %-30s %-9s %-9s %-8s %-6s %s" %
          ("card", "bdf", "name", "link", "GB/s est", "VRAM", "out", "note"))
    print("-" * 118)
    for g in db["gpus"]:
        L = g["link"]
        vr = "%s MiB" % g["vram_total_mib"] if g["vram_total_mib"] else "-"
        note = ""
        if g.get("integrated"):
            note = "INTEGRATED — shares system RAM; 'link' is the CPU root port, not a card link"
        elif L["capped_by_topology"]:
            note = "card says x%s, HOST gives x%s" % (L["own_reports_width"], L["max_width"])
        elif (L["max_width"] or 99) < 4:
            note = "NARROW — batch/offline card"
        if L.get("idle_degraded"):
            note += "  [idle now: x%s %s]" % (L["current_width"], L["current_gen"])
        print("%-6s %-13s %-30s %-9s %-9s %-8s %-6s %s" % (
            g["card"], g["bdf"], g["name"][:30],
            ("x%s %s" % (L["max_width"], L["max_gen"])) if L["max_width"] else "-",
            L["gbps_max"] or "-", vr, "yes" if g["has_output"] else "no", note))
    print("\nPlanning law: a secondary may take work only if it crosses the link ONCE or at")
    print("FRAME rate toward the display. Per-frame PIXEL offload is dead on links < ~25 GB/s.")


# ── doctor: verify THIS machine's readiness by testing the tool's own claims ──────────
def _isolation_count(vid, did):
    """How many GPUs a child process sees with the exclusive device-select applied. None = untestable."""
    if not shutil.which("vulkaninfo"):
        return None
    env = dict(os.environ)
    env["MESA_VK_DEVICE_SELECT"] = "%s:%s!" % (vid, did)
    try:
        out = subprocess.run(["vulkaninfo", "--summary"], capture_output=True, text=True,
                             env=env, timeout=30).stdout
        return out.count("deviceName")
    except Exception:
        return None


def doctor(db, as_json=False):
    """Run the tool's own claims against the live machine. PASS / FAIL / UNKNOWN, never a silent pass."""
    checks = []

    # A. the probe reads a link for every card
    miss = [g["card"] for g in db["gpus"] if g["link"]["gbps_max"] is None]
    checks.append({"check": "probe-links", "verdict": "FAIL" if miss else "PASS",
                   "detail": ("missing links: " + ",".join(miss)) if miss else "a host link for every card"})

    # B. routing ISOLATES on every card (the claim that was once false)
    if not shutil.which("vulkaninfo"):
        checks.append({"check": "routing-isolation", "verdict": "UNKNOWN",
                       "detail": "vulkaninfo not installed — cannot test"})
    else:
        for g in db["gpus"]:
            n = _isolation_count(g["vendor"][2:], g["device"][2:])
            if n is None:
                v, d = "UNKNOWN", "could not run a test child"
            elif n == 1:
                v, d = "PASS", "a child sees exactly 1 device"
            else:
                v, d = "FAIL", "a child sees %d devices (expected 1)" % n
            checks.append({"check": "routing-isolation", "card": g["card"], "verdict": v, "detail": d})

    # C. telemetry is readable (the monitor depends on it)
    n_ok = sum(1 for g in db["gpus"]
               if _int("/sys/class/drm/%s/device/mem_info_vram_used" % g["card"]) is not None)
    checks.append({"check": "telemetry", "verdict": "PASS" if n_ok == len(db["gpus"]) else "FAIL",
                   "detail": "VRAM counters readable on %d/%d cards" % (n_ok, len(db["gpus"]))})

    # D. the role plan places at least one card, and says why for the rest
    rp = assign_roles(db, "game")
    placed = sum(1 for r in rp if r["roles"] != ["idle"])
    unexplained = [r["card"] for r in rp if r["roles"] == ["idle"] and not r["why"]]
    checks.append({"check": "role-plan", "verdict": "PASS" if placed and not unexplained else "FAIL",
                   "detail": "places %d/%d cards%s" % (placed, len(rp),
                             ("" if not unexplained else "; unexplained idle: " + ",".join(unexplained)))})

    # E. UPSCALING — the path the tool advertises actually exists on this machine.
    gs_state, gs_detail = _gamescope_state()
    checks.append({"check": "upscaling",
                   "verdict": "PASS" if gs_state == "READY" else "UNKNOWN",
                   "detail": "%s — %s" % (gs_state, gs_detail)})

    # F. FRAME GENERATION — reported honestly; UNKNOWN is never green, and absent is not a failure
    #    of THIS machine (it needs software the user must own). Only a broken install is a FAIL.
    ls_state, ls_detail = _lsfg_state()
    ls_verdict = {"READY": "PASS", "NOT-WORKING": "FAIL"}.get(ls_state, "UNKNOWN")
    checks.append({"check": "frame-generation", "verdict": ls_verdict,
                   "detail": "%s — %s" % (ls_state, ls_detail)})

    bad = any(c["verdict"] == "FAIL" for c in checks)
    overall = "READY" if not bad else "PROBLEMS FOUND"

    if as_json:
        print(json.dumps({"spirit": "doctor/0.1", "overall": overall, "checks": checks}, indent=2))
        return 1 if bad else 0

    print("spirit doctor — verifying this machine's multi-GPU readiness\n")
    for c in checks:
        card = ("%-6s " % c["card"]) if c.get("card") else ""
        print("  [%-7s] %s%s" % (c["verdict"], card, c["detail"]))
    print("\n  OVERALL: %s%s" % (overall, "" if not bad else " — see FAIL lines above"))
    return 1 if bad else 0


# ── fg: can THIS card actually carry frame-gen / upscale for a game? ────────────────────
# Measured on the dev box 2026-10-09 (dma_buf bridge, RADV, pipelined, 64 MiB): the import lands in a
# HOST-VISIBLE aperture heap, so these are aperture rates, NOT peer-VRAM rates. Re-measure on any
# other machine — the numbers are a property of the LINKS, not of the tool.
HANDOFF_GBPS = {"card0": 0.76, "card2": 0.25, "card3": 2.91}
FRAME_MIB = {"1080p": 8.29, "1440p": 14.75, "4k": 33.18}


def fg(db, res="1440p60", mode="present-on-secondary"):
    """Frame budget arithmetic: can a rendered frame reach this card in time, one way or round trip?"""
    try:
        r, hz = res.rsplit("@", 1) if "@" in res else (res[:-2], res[-2:])
    except Exception:
        r, hz = "1440p", "60"
    r = r.rstrip("@")
    res = r if r in FRAME_MIB else "1440p"
    try:
        hz = int(hz)
    except ValueError:
        hz = 60
    payload = FRAME_MIB[res]
    budget = 1000.0 / hz
    one_way = (mode == "present-on-secondary")

    print("spirit fg — can a card carry the frame-gen/upscale pass for %s@%d?\n" % (res, hz))
    print("  frame payload: %.2f MiB    frame budget: %.2f ms\n" % (payload, budget))
    print("  %-8s %-12s %-14s %-12s %s" % ("card", "handoff", "one-way ms", "round-trip ms", "verdict"))
    print("  " + "-" * 84)
    for g in db["gpus"]:
        c = g["card"]
        rate = HANDOFF_GBPS.get(c)
        if rate is None:
            print("  %-8s %-12s %-14s %-12s %s" % (c, "n/a", "-", "-", "not a card pair here / not measured"))
            continue
        ow = payload / 1024.0 / rate * 1000.0
        rt = ow * 2
        if one_way:
            ok = ow <= budget
            verdict = "FEASIBLE one-way" if ok else "INFEASIBLE one-way"
            arith = "%.1f ms vs %.1f ms budget" % (ow, budget)
        else:
            ok = rt <= budget
            verdict = "FEASIBLE round-trip" if ok else "INFEASIBLE round-trip"
            arith = "%.1f ms vs %.1f ms budget" % (rt, budget)
        print("  %-8s %-12s %-14.2f %-12.2f %s (%s)" % (c, "%.2f GB/s" % rate, ow, rt, verdict, arith))
    print("\n  ★ ARCHITECTURE RULE, from measurement: put the DISPLAY on the card doing the frame-gen, so a")
    print("    frame crosses ONE WAY. A round trip doubles the cost and is the first thing to fail.")
    print("  ★ The handoff table above is MEASURED ON THE DEV BOX and is a property of its links — on any")
    print("    other machine re-measure it before trusting a verdict. RADV lands the import in a")
    print("    HOST-VISIBLE aperture heap, so these are aperture rates, not peer-VRAM rates.")
    return 0



# ── boost: what extra frames can THIS machine give you, right now ───────────────────────
# Two ways to more frames, and they are NOT the same thing:
#   UPSCALING  — render below native and scale up. Fewer pixels rendered. Real FPS, any card,
#                any Vulkan game. We drive gamescope's FSR 1.0 (permissive) for this.
#   FRAME GEN  — interpolate extra frames. The big multiplier. We do NOT ship an implementation
#                and we do NOT ship anyone else's: we detect lsfg-vk, which requires the user to
#                own Lossless Scaling, and drive it if it is there. See the README.
LSFG_LAYER = "/usr/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json"
LSFG_CONF = os.path.expanduser("~/.config/lsfg-vk/conf.toml")
LSFG_APPID = "993090"
STEAM_MANIFEST = os.path.expanduser("~/.local/share/Steam/steamapps/appmanifest_%s.acf" % LSFG_APPID)
LOSSLESS_DIR = os.path.expanduser("~/.local/share/Steam/steamapps/common/Lossless Scaling")

# ★ THE WITNESS — the only instrument that can see an INSERTED frame.
# Our own Vulkan layer, installed system-wide, INERT unless SPIRIT_PRESENT_COUNT names a file.
# It counts vkQueuePresentKHR calls from BELOW lsfg-vk in the layer chain, so it sees the images
# frame generation added. Nothing else can: the game's own counter counts frames IT rendered
# (measured: 23 with frame gen on, 23 with it off, identical, twice); an overlay sits above the
# layer for the same reason; and the compositor's scan-out counter reports the display refresh,
# which never changes. Source and rationale: present-counter/spirit_present_counter.c.
COUNTER_LAYER = "/usr/share/vulkan/implicit_layer.d/VkLayer_SPIRIT_present_counter.json"
COUNTER_LIB = "/usr/local/lib/libspirit_present_counter.so"


def _counter_state():
    """(state, detail) for the present counter. READY / PARTIAL / ABSENT — never a silent pass."""
    if not os.path.exists(COUNTER_LAYER):
        return "ABSENT", ("the present counter layer is not installed, so frame-gen output can only "
                          "be ASSERTED, not measured")
    if not os.path.exists(COUNTER_LIB):
        return "PARTIAL", "manifest is present but %s is missing" % COUNTER_LIB
    return "READY", "present counter installed — frame-gen output is measurable"


def _read_counter(path):
    """The counter file is a fixed-width decimal the layer rewrites on every present."""
    try:
        with open(path) as fh:
            raw = fh.read().strip()
        return int(raw) if raw else 0
    except (OSError, ValueError):
        return 0


def _gamescope_state():
    """(state, detail). READY / ABSENT / PARTIAL / UNKNOWN — never a silent pass."""
    if not shutil.which("gamescope"):
        return "ABSENT", "gamescope is not installed"
    try:
        r = subprocess.run(["gamescope", "--help"], capture_output=True, text=True, timeout=15)
    except Exception as exc:
        return "UNKNOWN", "gamescope --help failed (%s)" % exc
    # ★ gamescope prints its ENTIRE help to STDERR, not stdout (measured: 0 lines on stdout,
    # all of it on stderr). Reading stdout only reported "no fsr filter" about a build that has
    # one — a confident answer about a stream that is empty by design.
    out = (r.stdout + "\n" + r.stderr).lower()
    if "fsr" in out:
        return "READY", "gamescope present; FSR 1.0 upscaling available"
    return "PARTIAL", "gamescope present but no fsr filter is listed in --help"


def _lsfg_state():
    """(state, detail) for lsfg-vk — checked against the surfaces that OWN the answer.

    ★ NOT `lsfg-vk-cli validate`: that only validates its own config file, and it reports a clean
    "valid" while the runtime cannot initialise at all. Measured 2026-10-10: with Lossless Scaling
    installed on Steam's DEFAULT branch, the layer loads, finds Lossless.dll, and then dies with
    "The specified shader DLL does not exist" — because the frame-gen shaders ship only on Steam's
    "lsfg-vk" BETA BRANCH. The manifest is what distinguishes a working install from a broken one.
    """
    stood = sorted(glob.glob(LSFG_LAYER + ".disabled*"))
    if stood and not os.path.exists(LSFG_LAYER):
        return "STOOD-DOWN", ("installed but disabled at the loader; enable with: sudo mv %s %s"
                              % (stood[-1], LSFG_LAYER))
    if not os.path.exists(LSFG_LAYER):
        return "ABSENT", "lsfg-vk is not installed"
    if not os.path.exists(LOSSLESS_DIR):
        return "NEEDS-GAME", "Lossless Scaling is not in the Steam library"
    if not os.path.exists(os.path.join(LOSSLESS_DIR, "Lossless.dll")):
        return "NEEDS-GAME", "no Lossless.dll in the Lossless Scaling install"
    try:
        man = open(STEAM_MANIFEST, encoding="utf-8", errors="replace").read()
    except OSError:
        man = ""
    low = man.lower()
    # ★ the manifest spells it "BetaKey"; a lowercase literal never matches. Compare folded.
    if "betakey" not in low or '"lsfg-vk"' not in low:
        return "NEEDS-BRANCH", ("Lossless Scaling is on Steam's DEFAULT branch; switch it to the "
                                "\"lsfg-vk\" beta (Steam > Lossless Scaling > Properties > Betas). "
                                "Without it the frame-gen shaders are absent and the layer fails "
                                "with \"The specified shader DLL does not exist\"")
    if not shutil.which("lsfg-vk-cli"):
        return "PARTIAL", "layer present but lsfg-vk-cli is missing"
    return "INSTALLED", "layer + the lsfg-vk branch are present (path not exercised until a game runs)"


def _native_res(db):
    """The preferred mode of a connected output on the monitor card, or None. Rootless sysfs read."""
    main = _main_card(db)
    best = None
    for path in sorted(glob.glob("/sys/class/drm/%s-*/modes" % main["card"])):
        try:
            first = open(path).read().split("\n")[0].strip()
        except OSError:
            continue
        m = re.match(r"^(\d+)x(\d+)$", first)
        if not m:
            continue
        w, h = int(m.group(1)), int(m.group(2))
        if w < 1024:
            continue
        if best is None or w * h > best[0] * best[1]:
            best = (w, h)
    return best


def _render_res(native, factor):
    """Render resolution for a scale factor, forced even (video encoders and FSR want even)."""
    # ★ ROUND, do not truncate: 1440 * 0.7 is 1007.9999999999999 in binary float, and int()
    # turned a clean 1008 into 1006. A resolution that is off by two looks like a typo, not a bug.
    w = max(2, int(round(native[0] * factor))) // 2 * 2
    h = max(2, int(round(native[1] * factor))) // 2 * 2
    return w, h


def _ensure_lsfg_profile(multiplier):
    """Make sure a profile exists for this multiplier. Returns (profile_name, note). Never silent."""
    name = "spirit %dx" % multiplier
    try:
        text = open(LSFG_CONF, encoding="utf-8").read()
    except OSError:
        return None, "no lsfg-vk config at %s — create a profile in lsfg-vk-ui first" % LSFG_CONF
    if ('name = "%s"' % name) in text:
        return name, "profile %r already present" % name
    block = ('\n[[profile]]\nactive_in = []\nflow_scale = 1.0\nmultiplier = %d\nname = "%s"\n'
             'override_present_mode = true\npacing_mode = "vsync"\nperformance_mode = false\n'
             'preserve_swapchain_image_count = false\n' % (multiplier, name))
    try:
        shutil.copy2(LSFG_CONF, LSFG_CONF + ".bak-spirit")
        with open(LSFG_CONF, "a", encoding="utf-8") as fh:
            fh.write(block)
    except OSError as exc:
        return None, "could not write the profile (%s)" % exc
    return name, "added profile %r (backup: %s.bak-spirit)" % (name, LSFG_CONF)


def boost(db):
    gs_state, gs_detail = _gamescope_state()
    ls_state, ls_detail = _lsfg_state()
    native = _native_res(db)

    print("spirit boost — what extra frames this machine can give you, right now\n")
    print("  UPSCALING — render below native and scale up. Fewer pixels rendered = real FPS.")
    print("    gamescope : %-11s %s" % (gs_state, gs_detail))
    if native:
        print("    display   : %dx%d native, on the monitor card" % native)
        for f in (0.9, 0.8, 0.7, 0.6):
            rw, rh = _render_res(native, f)
            print("      --scale %.1f  ->  render %dx%d  (%.0f%% of the pixels)"
                  % (f, rw, rh, 100.0 * f * f))
    else:
        print("    display   : no connected output's preferred mode could be read")
    print()
    print("  FRAME GENERATION — interpolated frames. The big multiplier, and the one that needs")
    print("  software we do not ship: lsfg-vk, which requires owning Lossless Scaling.")
    print("    lsfg-vk   : %-11s %s" % (ls_state, ls_detail))
    c_state, c_detail = _counter_state()
    print("    witness   : %-11s %s" % (c_state, c_detail))
    print("                (our own present counter, below the frame-gen layer — the only")
    print("                 instrument that can see an INSERTED frame; without it a frame-gen")
    print("                 claim is an assertion, not a measurement)")
    print()
    if gs_state == "READY":
        print("  ⇒ Upscaling is available NOW:   spirit play --scale 0.7 -- <your game>")
    else:
        print("  ⇒ Upscaling is not ready: %s" % gs_detail)
    if ls_state == "INSTALLED":
        print("  ⇒ Frame generation is available: spirit play --framegen 2 -- <your game>")
    else:
        print("  ⇒ Frame generation is not ready: %s" % ls_detail)
    print("\n  ★ Neither one is a miracle, and this tool will not pretend otherwise. Upscaling adds")
    print("    frames by rendering fewer pixels. Frame generation adds frames by inventing them,")
    print("    and it can look wrong on fast motion — that is the technology, not this tool.")
    return 0


# ── play: the game path — the strongest card renders, every other card works the tail ──
def _main_card(db):
    """The card that feeds the monitor MUST render (the monitor law). Fallback: the top scorer."""
    panel = [g for g in db["gpus"] if g.get("has_output")]
    return max(panel or db["gpus"], key=score)


def _start_test_load(node):
    """A SYNTHETIC load on a spare card: ffmpeg's OWN generated test pattern, piped to /dev/null.

    ★ WHY THIS IS NAMED A LOAD AND NOT A JOB (2026-10-10 — the owner asked "are we offloading REAL JOBS
    onto the other GPUs?", and the honest answer was no). This function used to be called
    `_start_encode`, and the README said the spare cards "get a job" and "do real work". The encoder
    silicon genuinely runs — that is why the busy counters move — but the input is `testsrc2`, a test
    card ffmpeg synthesises, and the output goes to `-f null`. Nothing anyone cares about is faster
    or finished because of it. **A load proves a card can be DRIVEN. A job produces something.**
    `--work FILE` is the job. Never describe this as one, in code, in output, or in the README.
    """
    return subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=60",
         "-vaapi_device", "/dev/dri/%s" % node,
         "-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-f", "null", "-"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "LIBVA_DRIVER_NAME": "radeonsi"})


def _start_work(node, src, outdir):
    """A REAL job on a spare card: transcode `src` with THAT card's encoder, to a real file.

    The honest version of the offload. The input is a file that exists, the output is a file the user
    can open, and the card doing the work is NOT the card their game is on. This is the shape the
    e-waste mission actually needs: an old, mismatched, lesser card doing useful work while you play,
    at no cost to the game.

    Returns `(out_path, Popen)`. Uses the card's VAAPI encoder (H.264); on this box every amdgpu
    exposes one, including the 2017 Vega 10 parts.
    """
    stem = os.path.splitext(os.path.basename(src))[0] or "job"
    out = os.path.join(outdir, "%s.%s.mp4" % (stem, os.path.basename(node)))
    return out, subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src,
         "-vaapi_device", "/dev/dri/%s" % node,
         "-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-b:v", "8M", out],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "LIBVA_DRIVER_NAME": "radeonsi"})


def _is_wine_target(cmd):
    """True if this is a Proton/Wine launch, where the mount-namespace wall MUST be skipped.

    ★ MEASURED 2026-10-10: `unshare -r` maps our uid to 0 inside, and Wine/Proton REFUSE TO RUN AS
    ROOT — the launch dies with `bin_steam.sh: Error: Cannot run as root user` before the game ever
    starts. So on any Proton title (i.e. most Windows games, which is most of the audience) our
    second wall breaks the launch outright. Detect it and fall back to the pin alone, which is the
    load-bearing half anyway, and SAY SO rather than silently weakening the wall.
    """
    # ★ NO ENV LOOKUP HERE. An earlier version also checked STEAM_COMPAT_DATA_PATH, which meant a
    # stray variable left in a shell silently disabled the wall for NATIVE games too — the same
    # ambient-state trap that let SPIRIT_DEVICES poison a whole session. Decide on the ARGV, which
    # is what actually describes the launch.
    for a in cmd:
        b = os.path.basename(str(a)).lower()
        if b.endswith(".exe") or "proton" in b or b == "wine" or b.startswith("wine"):
            return True
    return False


def _steam_root():
    """The Steam install that has a `steamapps` directory, or None. Never guessed from a path shape."""
    for c in (os.path.expanduser("~/.steam/steam"), os.path.expanduser("~/.local/share/Steam")):
        if os.path.isdir(os.path.join(c, "steamapps")):
            return c
    return None


def _appid_for_cmd(cmd):
    """The Steam appid that owns this launch, resolved through the manifest's `installdir`.

    ★ NOT PARSED OFF THE PATH SPELLING. A game folder can be called anything, and the number in a
    launch line may be an argument to somebody else's script. The manifest is the surface that owns
    the mapping: `appmanifest_<appid>.acf` carries `"installdir" "<folder>"`, and that is what is
    matched. Returns None rather than a guess — an appid invented here would create the wrong
    directory, which is worse than creating none.
    """
    steam = _steam_root()
    if not steam:
        return None
    apps = os.path.join(steam, "steamapps")
    wanted = set()
    for a in cmd:
        m = re.search(r"steamapps/common/([^/]+)/", str(a))
        if m:
            wanted.add(m.group(1))
    if not wanted:
        return None
    for mf in sorted(glob.glob(os.path.join(apps, "appmanifest_*.acf"))):
        try:
            txt = open(mf, errors="replace").read()
        except OSError:
            continue
        im = re.search(r'"installdir"\s*"([^"]+)"', txt)
        if im and im.group(1) in wanted:
            return os.path.basename(mf)[len("appmanifest_"):-len(".acf")]
    return None


def _ensure_compatdata(cmd):
    """Create `steamapps/compatdata/<appid>/` when it is absent, before a Proton launch.

    ★ WHY, AND WHY IT BELONGS IN THE TOOL (traced to Proton's own source, 2026-10-10). Proton's
    `setup_prefix()` does `with self.prefix_lock:` — and that lock file lives INSIDE
    `compatdata/<appid>/`. If the DIRECTORY does not exist, Proton dies with a bare
    `FileNotFoundError: .../pfx.lock` and CANNOT create it, because creating it is the step it was
    about to fail on. So the first-ever launch of any Proton title through this tool fails with a
    Python traceback from inside Proton, which is a terrible first impression for a stranger and
    looks like our bug.

    One `os.makedirs` is the whole fix. It is a no-op for a title that has run before, so it is safe
    to call unconditionally, and it says plainly when it just made a first-ever launch work.
    """
    if not _is_wine_target(cmd):
        return
    appid = _appid_for_cmd(cmd)
    if not appid:
        return
    steam = _steam_root()
    if not steam:
        return
    d = os.path.join(steam, "steamapps", "compatdata", appid)
    if os.path.isdir(os.path.join(d, "pfx")):
        return                      # a real prefix is already there; touch nothing
    made = not os.path.isdir(d)
    try:
        os.makedirs(d, exist_ok=True)
    except OSError as e:
        print("  prefix: could not create %s (%s)" % (d, e))
        return
    if made:
        print("  prefix: FIRST-EVER LAUNCH of appid %s — created %s" % (appid, d))
        print("          (Proton's own prefix setup crashes on a missing compatdata dir; it cannot")
        print("           create the directory it needs, because its lock file lives inside it)")


def _hidden_argv(cmd, main, db):
    """[build] the game argv with the OTHER cards' /dev/dri nodes hidden behind a mount namespace.

    Two walls, both measured on the dev box: the EXCLUSIVE device-select (a child then sees exactly
    one GPU) and a mount namespace that makes the other cards' nodes invisible. The pin alone is
    load-bearing — WITHOUT it a Vulkan game chose the docked card outright on our box (2026-10-09).

    ★ KEEP THE NAMESPACE INSIDE ANY OUTER WRAPPER. `unshare -r` maps our uid to root, so the
    ROOT-OWNED `/tmp/.X11-unix` presents as the overflow uid inside — and a nested gamescope then
    dies at `xwayland/sockets.c:83 ... not owned by root or us`, followed by "Failed to create
    Xwayland server". Measured 2026-10-10. gamescope goes OUTSIDE this function; only the game is
    inside. Build here, call from the caller.
    """
    if _is_wine_target(cmd):
        return list(cmd)
    hide = []
    for g in db["gpus"]:
        if g["card"] == main["card"]:
            continue
        for n in (g["card"], g.get("render_node")):
            if n:
                hide.append(n)
    if hide and shutil.which("unshare"):
        script = ('hide="$1"; shift; for n in $hide; do '
                  '[ -e "/dev/dri/$n" ] && mount --bind /dev/null "/dev/dri/$n" 2>/dev/null || true; '
                  'done; exec "$@"')
        return ["unshare", "-rm", "sh", "-c", script, "sh", " ".join(hide)] + list(cmd)
    return list(cmd)


def _spawn_hidden(cmd, env, main, db):
    return subprocess.call(_hidden_argv(cmd, main, db), env=env)


def play(db, cmd, do_offload=True, scale=None, framegen=None, overlay=False,
         work=None, work_outdir=None):
    if not cmd:
        print("usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] "
              "[--work FILE [--work-outdir DIR]] -- <game command...>", file=sys.stderr)
        return 1
    main = _main_card(db)
    spares = [g for g in db["gpus"] if g["card"] != main["card"]]

    print("spirit play — the game on the strongest card, every other card working the tail\n")
    L = main["link"]
    print("  MAIN  (renders)  %-6s %-30s x%s %s  %s GB/s" %
          (main["card"], main["name"][:30], L["max_width"], L["max_gen"], L["gbps_max"]))
    for g in spares:
        print("  SPARE (offload)  %-6s %-30s node /dev/dri/%s" %
              (g["card"], g["name"][:30], g.get("render_node") or "?"))

    env = dict(os.environ)
    env["DRI_PRIME"] = "pci-%s" % main["bdf"]
    env["MESA_VK_DEVICE_SELECT"] = "%s:%s!" % (main["vendor"][2:], main["device"][2:])
    print("\n  pin: MESA_VK_DEVICE_SELECT=%s   DRI_PRIME=%s" %
          (env["MESA_VK_DEVICE_SELECT"], env["DRI_PRIME"]))

    # UPSCALING: wrap the game in gamescope and scale it up with FSR 1.0.
    if scale:
        gs_state, gs_detail = _gamescope_state()
        native = _native_res(db)
        if gs_state != "READY":
            print("  scale: REFUSED — %s" % gs_detail)
            return 1
        if not native:
            print("  scale: REFUSED — could not read the native mode of the monitor card's output")
            return 1
        rw, rh = _render_res(native, scale)
        print("  scale: render %dx%d -> present %dx%d via gamescope FSR 1.0  (%.0f%% of the pixels)"
              % (rw, rh, native[0], native[1], 100.0 * scale * scale))

    # FRAME GENERATION: drive lsfg-vk if it is here. We never ship or modify it.
    if framegen:
        st, detail = _lsfg_state()
        # ★ "READY" was renamed to "INSTALLED" when this check moved to the Steam manifest.
        # A rename that misses one caller is exactly how a working feature reports itself broken.
        if st != "INSTALLED":
            print("  framegen: UNAVAILABLE — %s" % detail)
            print("            (launching without it; spirit boost explains the state)")
        else:
            prof, note = _ensure_lsfg_profile(framegen)
            if prof:
                env["LSFGVK_PROFILE"] = prof
                if scale or overlay:
                    # ★ lsfg-vk's own troubleshooting: under gamescope with vsync pacing, set
                    # ENABLE_GAMESCOPE_WSI=0 or frame generation does not engage at all. This applies
                    # whenever gamescope is in play — including --overlay alone, with no upscaling.
                    env["ENABLE_GAMESCOPE_WSI"] = "0"
                print("  framegen: %dx via lsfg-vk, profile %r — %s" % (framegen, prof, note))
            else:
                print("  framegen: UNAVAILABLE — %s" % note)

    # ★ ARM THE WITNESS. Only meaningful with frame gen on; harmless otherwise, and we say so
    # plainly if it is missing rather than reporting a frame-gen claim we could not measure.
    fg_count_path = None
    if framegen:
        cst, cdet = _counter_state()
        if cst == "READY":
            fg_count_path = os.path.join(tempfile.gettempdir(), "spirit-presents-%d.txt" % os.getpid())
            try:
                os.unlink(fg_count_path)
            except OSError:
                pass
            env["SPIRIT_PRESENT_COUNT"] = fg_count_path
            print("  witness: counting images presented to the compositor (summary at exit)")
        else:
            print("  witness: UNAVAILABLE — %s" % cdet)

    if _is_wine_target(cmd):
        print("  wall: Proton/Wine detected — skipping the mount namespace (Wine refuses to run as")
        print("        root, and `unshare -r` makes us root); the exclusive pin still applies.")
        # ★ Before the launch, make sure Proton has somewhere to put its prefix. A first-ever
        # launch otherwise dies inside Proton with a bare FileNotFoundError on a lock file in a
        # directory Proton cannot create. Cheap, idempotent, and it speaks only when it mattered.
        _ensure_compatdata(cmd)

    procs = []
    job_outputs = []
    if do_offload and shutil.which("ffmpeg"):
        if work:
            # ★ A REAL JOB. Real input, real output, on cards that are NOT the one running the game.
            if not os.path.isfile(work):
                print("  work: REFUSED — %r is not a file. A job needs a real input; the synthetic "
                      "test load is what runs when you do not give one." % work)
                return 1
            outdir = os.path.abspath(work_outdir or ".")
            try:
                os.makedirs(outdir, exist_ok=True)
            except OSError as e:
                print("  work: REFUSED — cannot use %s (%s)" % (outdir, e))
                return 1
            for g in spares:
                n = g.get("render_node")
                if not n:
                    continue
                out, p = _start_work(n, work, outdir)
                procs.append(p)
                job_outputs.append((os.path.basename(n), out))
            if procs:
                print("  ★ JOB: transcoding %s on %d spare card(s) — real input, real output."
                      % (os.path.basename(work), len(procs)))
                for node, out in job_outputs:
                    print("      %s -> %s" % (node, out))
        else:
            for g in spares:
                n = g.get("render_node")
                if n:
                    procs.append(_start_test_load(n))
            if procs:
                print("  cards: %d spare card(s) running a SYNTHETIC TEST LOAD" % len(procs))
                print("         (ffmpeg's own testsrc2 test pattern -> /dev/null). This proves the")
                print("         cards can be DRIVEN; it is not a job and produces nothing.")
                print("         For real work: spirit play --work <file> [--work-outdir DIR] -- <game>")
    print("\n  launching…\n")
    sys.stdout.flush()

    inner = _hidden_argv(cmd, main, db)
    if overlay and not scale:
        gs_state, gs_detail = _gamescope_state()
        if gs_state != "READY" or not _native_res(db):
            print("  overlay: REFUSED - gamescope not ready (%s)" % gs_detail)
            return 1
    if scale or overlay:
        nat = _native_res(db)
        rw, rh = _render_res(nat, scale) if scale else nat
        # ★ -w/-h = the GAME's render size; -W/-H = the OUTPUT. These were SWAPPED, which made
        # --scale a silent no-op: the game rendered at native resolution and gamescope downscaled
        # the picture instead of upscaling it. Measured 2026-10-10 via lsfg-vk's own debug line
        # ("Building pipeline for 256Op x 144Op") while --scale 0.7 was supposedly active.
        gs = ["gamescope", "-w", str(rw), "-h", str(rh),
              "-W", str(nat[0]), "-H", str(nat[1]), "-f"]
        if scale:
            gs += ["-F", "fsr", "--fsr-sharpness", "2"]
        if overlay:
            # mangoapp inside gamescope — an on-screen FPS for the human to watch.
            # ★ It is NOT a witness. mangoapp reads the compositor's SCAN-OUT, which is the display
            # refresh and never changes when frame gen engages. The witness is the present counter
            # armed below (SPIRIT_PRESENT_COUNT); do not quote mangoapp's figure as proof of frames.
            gs += ["--mangoapp"]
            print("  overlay: mangoapp inside gamescope — for the eye, not the evidence")
            sys.stdout.flush()   # ★ else it is block-buffered when redirected and lands AFTER the child's output
        inner = gs + ["--"] + inner
    t0 = time.time()
    try:
        rc = subprocess.call(inner, env=env)
    finally:
        # ★ A TEST LOAD IS A LOAD AND DIES WITH THE GAME. A JOB IS NOT: terminating a half-finished
        # transcode leaves a truncated file and calls it done, which is the corrupt-quietly failure
        # this whole project has a law about. So jobs are WAITED FOR, then each output is checked for
        # existence and size on disk — the artifact, not the process's word for it.
        if work:
            for p in procs:
                try:
                    p.wait()
                except Exception:
                    pass
        else:
            for p in procs:
                p.terminate()
    elapsed = max(time.time() - t0, 1e-6)
    if work:
        print("\n  game exited (rc=%s) — the job ran to completion (not stopped with the game)" % rc)
        print("  ── JOB OUTPUT " + "─" * 56)
        for node, out in job_outputs:
            if os.path.isfile(out):
                print("  %-14s %s  (%.1f MB)" % (node, out, os.path.getsize(out) / 1048576.0))
            else:
                print("  %-14s MISSING — %s was not written. The job failed; do not report it as done."
                      % (node, out))
    else:
        print("\n  game exited (rc=%s) — test load stopped" % rc)

    if fg_count_path:
        n = _read_counter(fg_count_path)
        print("\n  ── WITNESS ─────────────────────────────────────────────────────────")
        print("  images presented to the compositor : %d" % n)
        print("  wall time                          : %.1f s" % elapsed)
        print("  presented rate                     : %.1f /s" % (n / elapsed))
        print("  ★ This is the number frame generation moves, and the ONLY number it moves.")
        print("    The game's own FPS counter cannot see it — that counts frames the GAME")
        print("    rendered, which frame gen does not change. Expect the game's figure to be")
        print("    LOWER with frame gen on: it is now paying for the interpolation.")
        print("    Honest limit: an image handed to the compositor is not proof it was scanned")
        print("    out, and no counter can tell you whether the interpolated frames look right.")
    return 0 if rc == 0 else rc


def main():
    argv = sys.argv[1:]
    # `run` needs to pass an arbitrary command through untouched (it may contain -i, --flags, `--`).
    # argparse's REMAINDER mangles option-like args, so parse `run` by hand.
    if argv and argv[0] == "run":
        role = card = None
        rest, i = [], 1
        while i < len(argv):
            if argv[i] == "--role":
                role = argv[i + 1]; i += 2
            elif argv[i] == "--card":
                card = argv[i + 1]; i += 2
            elif argv[i] == "--":
                rest = argv[i + 1:]; break
            else:
                rest = argv[i:]; break
        if not role or not card:
            print("usage: spirit run --role <role> --card <card> [-- <cmd>...]", file=sys.stderr)
            return 1
        return run(probe(), role, card, rest)

    # `play` also takes an arbitrary game command — parse by hand, same reason as `run`.
    if argv and argv[0] == "play":
        offload, rest, i, scale, framegen, overlay = True, [], 1, None, None, False
        work, work_outdir = None, None
        while i < len(argv):
            if argv[i] == "--no-offload":
                offload = False; i += 1
            elif argv[i] == "--work":
                # ★ REAL WORK, not the synthetic test load. The input must exist; a job that
                # cannot name what it is working on is not a job.
                if i + 1 >= len(argv):
                    print("spirit play: --work needs a file, e.g. --work capture.mkv", file=sys.stderr)
                    return 2
                work = argv[i + 1]; i += 2
            elif argv[i] == "--work-outdir":
                if i + 1 >= len(argv):
                    print("spirit play: --work-outdir needs a directory", file=sys.stderr)
                    return 2
                work_outdir = argv[i + 1]; i += 2
            elif argv[i] == "--scale":
                try:
                    scale = float(argv[i + 1])
                except (IndexError, ValueError):
                    print("spirit play: --scale needs a number, e.g. --scale 0.7", file=sys.stderr)
                    return 2
                i += 2
            elif argv[i] == "--framegen":
                try:
                    framegen = int(argv[i + 1])
                except (IndexError, ValueError):
                    print("spirit play: --framegen needs 2, 3 or 4", file=sys.stderr)
                    return 2
                i += 2
            elif argv[i] == "--overlay":
                overlay = True; i += 1
            elif argv[i] == "--":
                rest = argv[i + 1:]; break
            else:
                rest = argv[i:]; break
        if scale is not None and not (0.3 <= scale <= 1.0):
            print("spirit play: --scale must be between 0.3 and 1.0", file=sys.stderr)
            return 2
        if framegen is not None and framegen not in (2, 3, 4):
            print("spirit play: --framegen must be 2, 3 or 4", file=sys.stderr)
            return 2
        return play(probe(), rest, offload, scale, framegen, overlay, work, work_outdir)

    ap = argparse.ArgumentParser(
        prog="spirit",
        description="Spiritual Successor — put every GPU to work\n"
                    "commands: probe · roles · plan · doctor · fg · boost · run · play  "
                    "(`run` and `play` take a command after `--`)",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("probe"); p.add_argument("--json", action="store_true")
    r = sub.add_parser("roles"); r.add_argument("--profile", default="game"); r.add_argument("--json", action="store_true")
    pl = sub.add_parser("plan"); pl.add_argument("--workload", default="1440p60-framegen")
    pl.add_argument("--vram-mib", type=int, default=None, help="VRAM the workload needs on its card")
    sub.add_parser("doctor")
    sub.add_parser("boost")
    f = sub.add_parser("fg")
    f.add_argument("--res", default="1440p60", help="resolution@refresh, e.g. 1440p60 / 4k120 / 1080p144")
    f.add_argument("--mode", default="present-on-secondary",
                   choices=["present-on-secondary", "round-trip"],
                   help="one-way (display on the frame-gen card) or round trip")
    a = ap.parse_args(argv)
    db = probe()
    if a.cmd == "fg":
        return fg(db, a.res, a.mode)
    if a.cmd == "doctor":
        return doctor(db)
    if a.cmd == "boost":
        return boost(db)
    if a.cmd == "plan":
        return plan(db, a.workload, a.vram_mib)
    if a.cmd == "roles":
        if a.json:
            print(json.dumps(assign_roles(db, a.profile), indent=2))
        else:
            print_roles(db, a.profile)
        return 0
    if a.cmd == "probe":
        if a.json:
            print(json.dumps(db, indent=2))
        else:
            print_table(db)
        return 0
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
