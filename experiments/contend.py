#!/usr/bin/env python3
"""contend.py — measure whether a GPU-BOUND main load on one card loses throughput when a second
card is busy. This exists because vkmark (35,000 fps) is CPU-bound and CANNOT measure GPU contention;
the main load here (4K Vulkan denoise) pins the card at 86-100% busy, so it can.

    contend.py --pairs 6                 # main load alone vs main load + offload, interleaved
"""
import argparse, os, re, statistics as st, subprocess, sys, time

FF = "ffmpeg"
SRC4K = "/tmp/sp_src4k.mp4"
SRC = "/tmp/sp_src.mp4"
XT_SEL = "1002:1234!"          # vendor:device id of the card to pin as the renderer (set your own)
WIN = 12


def main_load(sec):
    # GPU-BOUND: 4K denoise on the Vulkan shaders, pinned to the XT
    return [FF, "-y", "-init_hw_device", "vulkan=vk:0", "-i", SRC4K, "-t", str(sec),
            "-vf", "format=yuv420p,hwupload,nlmeans_vulkan=s=4:p=3:r=7", "-f", "null", "-"]


def offload(sec, node):
    return [FF, "-y", "-stream_loop", "-1", "-i", SRC, "-t", str(sec),
            "-vf", "format=nv12,hwupload", "-vaapi_device", "/dev/dri/%s" % node,
            "-c:v", "h264_vaapi", "-f", "null", "-"]


def fps(out):
    m = re.findall(r"frame=\s*(\d+).*?fps=\s*([0-9.]+)", out)
    return float(m[-1][1]) if m else None


def run_main(with_offload, node):
    env = {**os.environ, "MESA_VK_DEVICE_SELECT": XT_SEL}
    p = subprocess.Popen(main_load(WIN), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    o = None
    if with_offload:
        time.sleep(2)
        o = subprocess.Popen(offload(WIN - 2, node), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, env={**os.environ, "LIBVA_DRIVER_NAME": "radeonsi"})
    out = p.communicate()[0]
    if o:
        o.communicate()
    return fps(out)


def stats(v):
    return "n=%d mean=%.1f sd=%.1f spread=%.1f%%" % (len(v), st.mean(v), st.pstdev(v),
                                                      100 * st.pstdev(v) / st.mean(v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", type=int, default=6)
    ap.add_argument("--node", default="renderD129", help="render node of the offload card")
    a = ap.parse_args()
    A, B = [], []
    for i in range(a.pairs):
        x = run_main(False, None)
        y = run_main(True, a.node)
        if x: A.append(x)
        if y: B.append(y)
        print("  pair %d: main-alone %.1f   main+offload %.1f" % (i + 1, x or -1, y or -1))
    print("\nMAIN LOAD ALONE          :", stats(A))
    print("MAIN LOAD + OFFLOAD      :", stats(B))
    if A and B:
        d = (st.mean(B) - st.mean(A)) / st.mean(A) * 100
        pooled = 100 * ((st.pstdev(A) + st.pstdev(B)) / 2) / st.mean(A)
        print("delta = %+.1f%%   pooled spread = %.1f%%" % (d, pooled))
        print("VERDICT:", "INSIDE NOISE - no claim" if abs(d) < pooled else "OUTSIDE NOISE - real")
    return 0


if __name__ == "__main__":
    sys.exit(main())
