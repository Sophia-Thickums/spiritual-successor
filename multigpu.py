#!/usr/bin/env python3
"""multigpu.py — the actual point of the project: MANY cards working AT ONCE.

Runs a heavy benchmark (Unigine Heaven 4K) on the main card while offloading a real VAAPI encode to
EVERY OTHER card, simultaneously. The monitor (run alongside) proves each card carried its own work.

    multigpu.py                     # offload to all non-main cards
    multigpu.py card0 card2         # offload to a chosen subset
    multigpu.py --list              # print the card -> render-node map

Copyright 2026 Sophia Marie DeClue. MIT.
"""
import argparse, os, re, subprocess, sys, time

HEAVEN_DIR = os.path.expanduser("~/.phoronix-test-suite/installed-tests/pts/unigine-heaven-1.6.6")
SRC = "/tmp/sp_src.mp4"
RUN_SEC = 290          # outlive the ~265 s Heaven pass


def node_map():
    """card -> render node, discovered from sysfs (never a remembered number)."""
    m = {}
    for c in ("card0", "card1", "card2", "card3"):
        try:
            for d in os.listdir("/sys/class/drm/%s/device/drm" % c):
                if d.startswith("renderD"):
                    m[c] = d
        except OSError:
            pass
    return m


def has_output(card):
    """True if a display is connected to this card (it is the presenter, not an offload target)."""
    try:
        import glob
        return any(open(p).read().strip() == "connected"
                   for p in glob.glob("/sys/class/drm/%s-*/status" % card))
    except OSError:
        return False


def enc(node):
    return subprocess.Popen(
        ["ffmpeg", "-y", "-stream_loop", "-1", "-i", SRC, "-t", str(RUN_SEC),
         "-vf", "format=nv12,hwupload", "-vaapi_device", "/dev/dri/%s" % node,
         "-c:v", "h264_vaapi", "-f", "null", "-"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "LIBVA_DRIVER_NAME": "radeonsi"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cards", nargs="*", help="cards to offload to (default: all non-main)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    nm = node_map()
    if a.list:
        for c, n in sorted(nm.items()):
            print("  %-6s -> /dev/dri/%s%s" % (c, n, "   (owns a panel)" if has_output(c) else ""))
        return 0

    main_card = next((c for c in nm if has_output(c)), None)
    targets = a.cards or [c for c in sorted(nm) if c != main_card]
    print("MAIN (benchmark): %s   OFFLOAD targets: %s" % (main_card, ", ".join(targets) or "none"))
    if not main_card:
        print("no card owns a display; refusing to guess the main GPU", file=sys.stderr)
        return 2

    procs = {c: enc(nm[c]) for c in targets if c in nm}
    print("  started %d concurrent offload(s): %s" % (len(procs), ", ".join(procs)))
    time.sleep(2)

    env = {**os.environ, "OS_ARCH": "x86_64", "LOG_FILE": "/tmp/multigpu_heaven.log"}
    t = time.time()
    subprocess.run(["./unigine-heaven", "-video_width", "3840", "-video_height", "2160",
                    "-video_fullscreen", "1"], cwd=HEAVEN_DIR, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=420)
    for p in procs.values():
        p.terminate()

    log = open("/tmp/multigpu_heaven.log").read()
    fps = re.findall(r"FPS:\s*([0-9.]+)", log)
    fr = re.findall(r"Frames:\s*([0-9]+)", log)
    print("\n  HEAVEN 4K with %d cards offloading: frames=%s  FPS=%s  (%.0fs)"
          % (len(procs), fr, fps, time.time() - t))
    return 0


if __name__ == "__main__":
    sys.exit(main())
