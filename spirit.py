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
import os, sys, json, glob, re, shutil, subprocess, argparse

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

    ap = argparse.ArgumentParser(prog="spirit", description="Spiritual Successor — put every GPU to work")
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("probe"); p.add_argument("--json", action="store_true")
    r = sub.add_parser("roles"); r.add_argument("--profile", default="game"); r.add_argument("--json", action="store_true")
    pl = sub.add_parser("plan"); pl.add_argument("--workload", default="1440p60-framegen")
    pl.add_argument("--vram-mib", type=int, default=None, help="VRAM the workload needs on its card")
    a = ap.parse_args(argv)
    db = probe()
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
