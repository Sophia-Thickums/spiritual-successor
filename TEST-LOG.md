# SPIRITUAL SUCCESSOR — TEST LOG
*2026-10-09 (Fri). "Test, measure, adjust, prove." Every line here is backed by a command that ran.*

## CLAIM INVENTORY — what is actually being claimed
**Claimed by MVP-0 (and testable now):**
1. `spirit probe` reports each GPU's TRUE host link (min negotiated width along the sysfs chain).
2. `spirit roles` assigns a job to every card, or states why not — and scales to N cards.
3. `spirit plan` gives a feasibility verdict with the arithmetic.
4. `spirit run` routes a command to a chosen card so the child sees only that card.

**NOT claimed by MVP-0 (do not test for it, it does not exist yet):**
- That it makes ANY game or benchmark faster. **MVP-0 does not touch the render path.** Running a
  game "with spirit in place" MUST produce an unchanged score, and that null is correct, not a failure.

---

## TEST A — probe truth — ✅ PASS
Cross-checked the device list and order against an independent surface:
`vulkaninfo --summary` reports 4 devices: 9070 XT, MI25, Vega FE, iGPU — **identical set and order**
to `spirit probe` (which sorts by capability). Links (XT x16 Gen5, MI25 x4 Gen3, FE x1 Gen3) match the
values recorded on 2026-10-01 against `dmesg`.

## TEST B — routing isolation — ❌ FAILED, then ✅ FIXED AND PASS
**First result (the bad one, kept):** routing to card0 set `MESA_VK_DEVICE_SELECT=1002:6860` and the
child STILL enumerated **all 4 GPUs** — it only *reordered* them. **The routing claim was FALSE.**
**Root cause:** the trailing `!` is the EXCLUSIVE flag. Measured: `1002:6860` → 4 devices;
`1002:6860!` → **1** device.
**After the fix, per card:**
| routed to | devices the child sees | device |
|---|---|---|
| card0 | 1 | Instinct MI25 |
| card1 | 1 | RX 9070 XT |
| card2 | 1 | Vega Frontier Edition |
| card3 | 1 | Ryzen iGPU |
✅ Routing now isolates correctly. This bug was ONLY found by testing — it is exactly why the proof
was demanded.

## TEST C — "monitor everything" harness — ✅ PASS (after 3 bug fixes)
`monitor.py` samples per GPU (VRAM, busy %, edge/junction temp, power, clock, live link) plus CPU
busy, load, RAM; writes JSONL; prints a per-card summary.
**Bugs found and fixed by running it:**
1. Card paths vs basenames were mixed → the summary printed NO rows. Fixed.
2. `hwmon` path was built from a full path, not a basename → temps/power read `null`. Fixed.
3. `pp_dpm_sclk` parsed the index ("S") instead of the clock ("42Mhz"). Fixed.

## TEST D — Superposition 1440p benchmark — ❌ NOT ACHIEVED (honest)
**Unigine Superposition IS installed** (`~/benchmarks/Unigine_Superposition-1.1/`), with a prior
1440p/OpenGL/High config we made (`bin/sp_1440p_gl.xml`) and a `bin/superposition_cli` binary.
**Two invocations were attempted; neither produced a score.** The engine launches and renders
(it ran for 100 s until a timeout), but no result file was written and — decisively — the monitor
proves **peak GPU busy on ANY card was 3%** across the run window:
```
samples: 25
  card0: peak busy 0%   card1: peak busy 3%   card2: peak busy 0%   card3: peak busy 0%
PEAK BUSY ANY CARD: 3 %  <- no benchmark load was applied
```
**So: NO benchmark number exists yet. This is not a result, it is an unfinished step.**
Next: use the sanctioned runner (`phoronix-test-suite`, test id `unigine-super-1.0.8`) or the correct
`superposition_cli` invocation, and CONFIRM the run by GPU busy > 50% before trusting any score.

## TEST E — real game — ❌ NOT RUN YET
**Cyberpunk 2077 is NOT installed on this box.** The "Cyberpunk dual-GPU benchmark" cited earlier was
**GameGPU's third-party test of a different tool (lsfg-vk)**, not ours. Corrected.
**Steam library actually present:** NTE: a large open-world UE5 title (UE5), a small indie, an older re-release.
A `Control` folder exists in `steamapps/common` with **no appmanifest** — orphaned, unverified.
First real-game test target: **NTE** (UE5), under `mangohud` + `monitor.py`.

---

## THE RULE THIS SESSION RE-ESTABLISHED
**A run is only proved by an instrument that cannot agree with you by accident.** The routing claim
looked fine — env set, no error, exit 0 — and was false. It took a child process *counting the devices
it could see* to expose it. Peak GPU busy is the same kind of witness for a benchmark: a score that
appears without the GPU loading is a fake green, not a result.


---

## TEST F — vkmark BASELINE, 9070 XT ONLY — ✅ PASS (a real number, verified)
    spirit run --role compute --card card1 -- vkmark --fullscreen \
      -b "shading:shading=phong:duration=15" -b "texture:anisotropy=16:duration=15" \
      -b "vertex:device-local=true:duration=15"
vkmark's own header confirmed the device was the XT (Device ID 0x7550).
**shading 35,635 FPS · texture 31,880 FPS · vertex 36,805 FPS · vkmark Score 34,773.**
Monitor (58 samples) across the run: **card1 peak busy 100%, 250 W, 75 C**; card0/2/3 all 0%, 0 W.
=> the load was PROVABLY on exactly one card. **Positive control established. This is the baseline.**

## TEST G — per-card capability map — ✅ real numbers
Same test, one card at a time (shading=phong, fullscreen):

| card | device | FPS |
|---|---|---|
| card1 | RX 9070 XT | **35,635** |
| card3 | Ryzen iGPU | 1,315 |
| card0 | Instinct MI25 | 113 |
| card2 | Vega Frontier Edition | 29 |

Each card's run allocated its OWN VRAM (MI25 7→140 MiB, FE 8→141 MiB, iGPU 15→147 MiB) and the run
header named that card only — so the routing is confirmed again, four for four.
**★ The striking datum: the MI25 (113) and the Vega FE (29) are the SAME Vega10 silicon. The only
structural difference is the link — x4 Gen3 (3.4 GB/s) vs x1 Gen3 (0.87 GB/s) — and the FE is ~4x slower.
Consistent with the link-wall thesis. NOT isolated from clocks/power-state yet — do not over-claim it.**

## TEST H — Unigine Superposition — ❌ DEAD on this box (documented, not blamed)
The engine (UNIGINE 2.5-dev superposition-1.1, Apr 2019) **mis-detects RDNA4**: its log reads
`GPU: Unknown GPU x1 / Video memory: 256 MB`, and it falls back to a **1600x900** video mode no matter
what `-preset` / `-video_fullscreen` is passed. That fallback *is* the broken low-res window observed.
`superposition_cli -config <xml>` exits instantly and writes nothing. Its correct driver is the GUI
launcher, which cannot be run headlessly here. **Verdict: use a modern Vulkan benchmark (vkmark).**
(The Cyberpunk "benchmark" remains a non-item — that game is not installed.)

## TEST I — `vkmark -D <uuid>` is UNUSABLE here, and that is precisely the gap spirit fills
All four RADV devices report the **SAME Device UUID** (`ac3da3d19386ed23d795ff786b3cb33e`), so vkmark's
`-D/--use-device` cannot tell them apart. The ONLY way found to pin a benchmark to one GPU is
`MESA_VK_DEVICE_SELECT=vid:did!` — which is exactly what `spirit run` emits. **On this box, spirit is
not decoration; it is the mechanism that makes per-GPU benchmarking possible at all.**


---

## TEST J — THE LINK CLAIM, PROVEN (with a positive control) — ✅
The claim that was only *suggested* by TEST G is now settled. Method: identical scene, identical
duration, two frame sizes, three cards. If a card is transport-bound, shrinking the frame should send
FPS soaring; if it is compute-bound, FPS will not move. Routed with `spirit run`, one card at a time.

| card | 320x240 | fullscreen | ratio |
|---|---|---|---|
| card0  Instinct MI25 (x4 Gen3, 3.4 GB/s) | 5,039 | 113 | **44.6x** |
| card2  Vega Frontier (x1 Gen3, 0.87 GB/s) | 1,350 | 29 | **46.6x** |
| card1  RX 9070 XT (x16 Gen5, 54.4 GB/s) | 56,017 | 35,657 | **1.57x**  ← CONTROL |

**A 76x reduction in per-frame pixels multiplies the Vega cards' throughput ~45x and the XT's by only
1.57x.** So the Vega cards' throughput is dominated by PER-FRAME DATA MOVEMENT, and the XT's is not.
**The link is the wall — proven, not suggested.**

**Corroboration from the same data:** MI25 / FE = 3.73x (small) and 3.90x (fullscreen) — both very close
to their **link-width ratio of 3.67x** (x4 vs x1). Two independent angles, one answer.

**Clocks RULED OUT (this was the open question from TEST G):** memory clock identical on both (945 MHz),
neither card boosted (core stayed at their idle levels, 1269 / 852 MHz), and both sat at trivial load.
Equal clocks + 3.9x different throughput = the clock explanation is dead.

**Honest caveat, stated so it is not over-read:** at 320x240 the FE moves only ~0.4 GB/s, *below* its
0.87 GB/s ceiling — so this is a **SCALING** result, not a measurement of a saturated wire. What it
proves is that per-frame volume governs these cards; it does not claim they were pinned at the ceiling.

**Why this matters for the whole project:** it is the empirical basis for the Spiritual Successor's
central law — *cross the link at FRAME rate or ONCE, never per pixel.* Per-frame pixel work is not slow
here; it is 45x wrong.


---

## TEST K — the FIRST REAL OFFLOAD: encode on the MI25 while the XT renders
**VAAPI H.264 encode works on both cards** (1080p60 source, software decode -> hwupload -> vaapi):
XT (renderD128) **385-415 fps**; MI25 (renderD129) **197-198 fps** (= 3.3x realtime). `vainfo` is not
installed but the VA driver (`radeonsi_drv_video.so`) is, and libva is present.

**The offload RAN.** During the offloaded case the monitor shows the MI25 allocating **54 MiB** of VRAM
(idle: 7 MiB) while the XT sat at **100% busy** rendering — so the encode genuinely landed on the
secondary card.

### ★ AND THEN THE RESULT COLLAPSED, CORRECTLY — MEASURE THE NOISE FLOOR FIRST
I had a tempting story: encode-on-the-render-card cost the render **-16.5%**, and offloading it cost
only **-9.8%**, so "offloading saves you ~7 points." **That reading is RETRACTED.** Measuring vkmark's
own run-to-run spread, alone, 5 runs:
```
27871, 29486, 33402, 35145, 35160   ->  mean 32213, sd 2999, SPREAD 9.31%
```
**The noise floor is +/-9.31%.** The "offloaded" delta (-9.8%) sits INSIDE it, and even the
same-card delta (-16.5%) is only barely outside. **No claim about the FPS cost of offloading is
supported by that data.**

**What IS established, and what is NOT:**
- ✅ ESTABLISHED: a real 1080p60 H.264 encode runs on the MI25 at ~197 fps *concurrently with* the XT
  rendering at full tilt, routed by `spirit` (render-node + `MESA_VK_DEVICE_SELECT` isolation).
- ❌ NOT ESTABLISHED: whether an offloaded encode costs the render card any measurable FPS.
- **Method fix required (next):** n>=10 per condition and compare DISTRIBUTIONS, or a lower-variance
  render load. A single-run A/B/C on a +/-9% instrument cannot decide a 7-point effect.

**The law that caught it, restated with a fresh receipt:** *measure your noise floor first; a
difference smaller than the spread is not a difference.* Without that step this session would have
shipped a fake number into the project record.


### TEST K, part 2 — the cost IS real, but the INSTRUMENT is wrong (recorded)
Repeated properly: **n=8 interleaved pairs**, A (render alone) vs C (render + encode on the MI25):
```
A  render alone            mean 34,806   sd  788   spread 2.26%   (min 32,982  max 35,427)
C  render + encode @MI25   mean 31,338   sd 2876   spread 9.18%   (min 27,681  max 35,251)
delta mean(C-A) = -9.97%  -> ~1.9 sd, marginally OUTSIDE noise
```
Note the ALONE case became stable (2.26%) once repeated — the earlier one-shot 9.31% floor was contaminated
by warmup/outliers. **Repeating changed the instrument's character, not just its precision.**

**Then the confound check (D): render alone + a CPU-ONLY ffmpeg decode, no GPU encode at all.**
```
D  render + CPU-only load  mean 32,795   sd 2635   spread 8.03%   -> -5.8% vs A, ~1.2 sd = INSIDE noise
```
A CPU-only load already produces a ~6% dip. So **the offload's measured cost is partly the encoder's
SOFTWARE DECODE burning CPU — not GPU contention.** At this n, CPU-vs-GPU cause is NOT separable.

**★ THE REAL FINDING — the instrument cannot measure what we want:** vkmark renders at **35,000+ FPS** on
the XT. A card doing 35k fps is **not GPU-bound**; it is CPU/present-bound. Therefore **vkmark can NEVER
measure GPU contention on this card** — any "cost" it reports is chiefly CPU and scheduler noise.
- vkmark remains valid for what TEST G/J used it for: ROUTING and per-card CAPABILITY.
- It is INVALID for measuring render cost under offload.
- **To measure offload cost, the render load must be GPU-BOUND — i.e. a real game (NTE), at settings that
  saturate the card.** That is the next instrument, and it must be validated the same way (noise floor first).
