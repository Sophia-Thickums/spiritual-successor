#!/usr/bin/env python3
"""offload3.py — run a real VAAPI H.264 encode on each named card CONCURRENTLY, for N seconds.

Used to hold the non-main cards busy while an external benchmark runs on the main card.

    offload3.py 250 card0 card2 card3
"""
import os, subprocess, sys, time

NODES = {"card0": "renderD129", "card1": "renderD128", "card2": "renderD130", "card3": "renderD131"}
SRC = "/tmp/sp_src.mp4"


def enc(node, secs):
    return subprocess.Popen(
        ["ffmpeg", "-y", "-stream_loop", "-1", "-i", SRC, "-t", str(secs),
         "-vf", "format=nv12,hwupload", "-vaapi_device", "/dev/dri/%s" % node,
         "-c:v", "h264_vaapi", "-f", "null", "-"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "LIBVA_DRIVER_NAME": "radeonsi"})


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 250
    cards = sys.argv[2:] or ["card0", "card2", "card3"]
    ps = {c: enc(NODES[c], secs) for c in cards if c in NODES}
    print("offload3: %d concurrent encodes on %s for %ds" % (len(ps), ", ".join(ps), secs), flush=True)
    time.sleep(secs)
    for p in ps.values():
        p.terminate()
    print("offload3: done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
