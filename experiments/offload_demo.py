#!/usr/bin/env python3
"""offload_demo.py — does offloading an encode to a SECONDARY card cost the render card anything?

Runs vkmark (a render load) on one card, and a VAAPI H.264 encode on another card AT THE SAME
TIME, then reports the render FPS and the encode rate. The comparison that matters:

    A) render alone                     -> the render card's ceiling
    B) render + encode ON THE SAME card -> the cost of doing the encode where you render
    C) render + encode ON A SECOND card -> the cost, offloaded

If C ~= A and B < A, the offload works and is worth shipping.

    offload_demo.py --render card1 --encode none                --seconds 20
    offload_demo.py --render card1 --encode card1 --node renderD128
    offload_demo.py --render card1 --encode card0 --node renderD129

Copyright 2026 Sophia Marie DeClue. MIT.
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SPIRIT = os.path.join(HERE, "spirit.py")
SRC = "/tmp/sp_src.mp4"
FFMPEG = "ffmpeg"


def render_cmd(card, seconds):
    return [sys.executable, SPIRIT, "run", "--role", "compute", "--card", card, "--",
            "vkmark", "--fullscreen", "-b", "shading:shading=phong:duration=%d" % seconds]


def encode_cmd(node, seconds):
    # software decode -> hwupload -> VAAPI encode, looped to stay busy for the window
    return [FFMPEG, "-y", "-stream_loop", "-1", "-i", SRC, "-t", str(seconds),
            "-vf", "format=nv12,hwupload", "-vaapi_device", "/dev/dri/%s" % node,
            "-c:v", "h264_vaapi", "-f", "null", "-"]


def fps_from(text):
    m = re.findall(r"frame=\s*(\d+).*?fps=\s*([0-9.]+)", text)
    return float(m[-1][1]) if m else None


def vkmark_fps(text):
    m = re.findall(r"FPS:\s*([0-9]+)", text)
    return int(m[-1]) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--render", required=True)
    ap.add_argument("--encode", default="none", help="card id, or 'none'")
    ap.add_argument("--node", default=None, help="renderDNNN for the encode card")
    ap.add_argument("--seconds", type=int, default=20)
    a = ap.parse_args()

    if not os.path.exists(SRC):
        print("no source clip at %s — make one first" % SRC, file=sys.stderr); return 2

    label = "render %s, encode %s" % (a.render, a.encode)
    print("RUN: %s  (%ds window)" % (label, a.seconds))

    env = dict(os.environ)
    if a.encode != "none":
        env["LIBVA_DRIVER_NAME"] = "radeonsi"

    rp = subprocess.Popen(render_cmd(a.render, a.seconds),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ep = None
    time.sleep(3)
    if a.encode != "none":
        ep = subprocess.Popen(encode_cmd(a.node, a.seconds - 3),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    rout = rp.communicate()[0]
    eout = ep.communicate()[0] if ep else ""

    rfps = vkmark_fps(rout)
    efps = fps_from(eout)
    print("  render FPS : %s" % (rfps if rfps else "-"))
    print("  encode FPS : %s" % (("%.0f" % efps) if efps else ("-" if a.encode == "none" else "FAILED")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
