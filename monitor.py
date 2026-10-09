#!/usr/bin/env python3
"""monitor.py — sample EVERYTHING, so a claim about a run can be checked against the run.

Per GPU: VRAM used, GPU busy %, edge/junction temp, power, current core clock, live PCIe link.
Whole box: CPU busy %, load average, RAM used, swap.
Writes JSONL (one object per sample) and prints a per-card summary on exit.

No dependencies. No root. Read-only.

    python3 monitor.py --seconds 30 --hz 2 --out run.jsonl --label baseline
"""
import os, sys, json, glob, time, argparse

CARD_RE = __import__("re").compile(r"^card\d+$")


def _r(p):
    try:
        with open(p) as f:
            return f.read().strip()
    except OSError:
        return None


def _i(p):
    v = _r(p)
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _hwmon(card):
    g = glob.glob("/sys/class/drm/%s/device/hwmon/hwmon*" % card)
    return g[0] if g else None


DRM = "/sys/class/drm"


def cards():
    cs = [os.path.basename(c) for c in glob.glob(DRM + "/card*") if CARD_RE.match(os.path.basename(c))]
    return sorted(cs, key=lambda c: int(c[4:]))


def gpu_sample(card):
    b = "%s/%s/device" % (DRM, card)
    hw = _hwmon(card)
    s = {
        "card": card,
        "vram_used_mib": (_i(b + "/mem_info_vram_used") or 0) // 1048576,
        "busy_pct": _i(b + "/gpu_busy_percent"),
        "edge_c": None, "junction_c": None, "power_w": None, "sclk_mhz": None,
        "link_w": _i(b + "/current_link_width"),
        "link_gt": _r(b + "/current_link_speed"),
    }
    if hw:
        t1 = _i(hw + "/temp1_input")
        t2 = _i(hw + "/temp2_input")
        pw = _i(hw + "/power1_average")
        s["edge_c"] = round(t1 / 1000, 1) if t1 else None
        s["junction_c"] = round(t2 / 1000, 1) if t2 else None
        s["power_w"] = round(pw / 1e6, 1) if pw else None
    pps = _r(b + "/pp_dpm_sclk")
    if pps:
        for line in pps.splitlines():
            if line.rstrip().endswith("*") and ":" in line:
                s["sclk_mhz"] = line.split(":", 1)[1].replace("*", "").strip()
    return s


def cpu_ram(prev):
    out = {}
    try:
        f = open("/proc/stat").readline().split()[1:]
        vals = [int(x) for x in f]
        idle = vals[3] + vals[4]
        total = sum(vals)
        if prev:
            dt, di = total - prev[0], idle - prev[1]
            out["cpu_busy_pct"] = round(100 * (1 - di / dt), 1) if dt else None
        prev[0], prev[1] = total, idle
    except Exception:
        pass
    try:
        mi = {}
        for line in open("/proc/meminfo"):
            k = line.split(":")[0]
            if k in ("MemTotal", "MemAvailable", "SwapTotal", "SwapFree"):
                mi[k] = int(line.split()[1])
        out["ram_used_mib"] = (mi["MemTotal"] - mi["MemAvailable"]) // 1024
        out["ram_total_mib"] = mi["MemTotal"] // 1024
    except Exception:
        pass
    try:
        out["load1"] = round(float(open("/proc/loadavg").read().split()[0]), 2)
    except Exception:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--hz", type=float, default=2)
    ap.add_argument("--out", default="monitor.jsonl")
    ap.add_argument("--label", default="run")
    a = ap.parse_args()

    cs = cards()
    prev, t0, n = [0, 0], time.time(), 0
    samples = []
    with open(a.out, "w") as fh:
        while time.time() - t0 < a.seconds:
            s = {"t": round(time.time() - t0, 2), "label": a.label,
                 "gpus": [gpu_sample(c) for c in cs]}
            s.update(cpu_ram(prev))
            fh.write(json.dumps(s) + "\n"); fh.flush()
            samples.append(s); n += 1
            time.sleep(max(0, 1.0 / a.hz))

    print("monitor — label=%s  samples=%d  over %.1fs  -> %s\n" % (a.label, n, a.seconds, a.out))
    print("%-7s %-11s %-11s %-11s %-9s %-9s" % ("card", "peak VRAM", "peak busy%", "peak temp", "peak W", "clock"))
    print("-" * 66)
    for c in cs:
        rows = [x for s in samples for x in s["gpus"] if x["card"] == c]
        if not rows:
            continue
        pv = max((r["vram_used_mib"] or 0) for r in rows)
        pb = max((r["busy_pct"] or 0) for r in rows)
        pt = max((r["junction_c"] or r["edge_c"] or 0) for r in rows)
        pw = max((r["power_w"] or 0) for r in rows)
        cl = next((r["sclk_mhz"] for r in reversed(rows) if r["sclk_mhz"]), "-")
        print("%-7s %-11s %-11s %-11s %-9s %-9s" % (c, "%d MiB" % pv, "%d%%" % pb,
              ("%.1fC" % pt) if pt else "-", ("%.1f" % pw) if pw else "-", cl))
    cpu = [s.get("cpu_busy_pct") for s in samples if s.get("cpu_busy_pct") is not None]
    ram = [s.get("ram_used_mib") for s in samples if s.get("ram_used_mib")]
    if cpu:
        print("\nbox: CPU busy peak %.1f%%  mean %.1f%%  |  RAM peak %d MiB" %
              (max(cpu), sum(cpu) / len(cpu), max(ram) if ram else 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
