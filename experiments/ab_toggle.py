#!/usr/bin/env python3
"""ab_toggle.py — alternate multi-card offload ON/OFF while a game runs, logging exact phase
boundaries so the frame-time CSV can be sliced by wall clock.

Writes /tmp/ab_phases.json:
    {"game_csv_start": null, "phases": [{"state":"on","start":<epoch>,"end":<epoch>}, ...]}

    ab_toggle.py --on 45 --off 45 --cycles 4 --cards card0 card2 card3
"""
import argparse, json, os, subprocess, sys, time

NODES = {"card0": "renderD129", "card1": "renderD128", "card2": "renderD130", "card3": "renderD131"}
SRC = "/tmp/sp_src.mp4"
OUT = "/tmp/ab_phases.json"


def enc(node):
    return subprocess.Popen(
        ["ffmpeg", "-y", "-stream_loop", "-1", "-i", SRC, "-t", "600",
         "-vf", "format=nv12,hwupload", "-vaapi_device", "/dev/dri/%s" % node,
         "-c:v", "h264_vaapi", "-f", "null", "-"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "LIBVA_DRIVER_NAME": "radeonsi"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--on", type=int, default=45)
    ap.add_argument("--off", type=int, default=45)
    ap.add_argument("--cycles", type=int, default=2)
    ap.add_argument("--cards", nargs="*", default=["card0", "card2", "card3"])
    a = ap.parse_args()

    phases = []
    print("ab_toggle: %d cycles of ON %ds / OFF %ds on %s" % (a.cycles, a.on, a.off, a.cards), flush=True)
    for c in range(a.cycles):
        # OFF phase first, so the run always OPENS with a clean baseline
        t = time.time()
        print("  [cycle %d] OFF  %s" % (c + 1, time.strftime("%H:%M:%S")), flush=True)
        time.sleep(a.off)
        phases.append({"state": "off", "start": t, "end": time.time()})

        ps = [enc(NODES[k]) for k in a.cards if k in NODES]
        t = time.time()
        print("  [cycle %d] ON   %s  (%d encodes)" % (c + 1, time.strftime("%H:%M:%S"), len(ps)), flush=True)
        time.sleep(a.on)
        for p in ps:
            p.terminate()
        phases.append({"state": "on", "start": t, "end": time.time()})

    json.dump({"phases": phases}, open(OUT, "w"), indent=1)
    print("ab_toggle: done -> %s" % OUT, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
