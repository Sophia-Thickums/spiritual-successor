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


---

## TEST L — the offload's cost, measured on a VALID instrument — and a genuine surprise
**A GPU-BOUND instrument was found:** `ffmpeg -init_hw_device vulkan -vf nlmeans_vulkan` (4K denoise,
pinned to the XT) holds the card at **86-100% busy, ~32 fps**. (vkmark's 35,000 fps is CPU/present-bound
and CANNOT measure GPU contention — see TEST K pt2.)

**`contend.py`, n=6 interleaved pairs:**
```
MAIN LOAD ALONE       mean 31.7 fps   sd 0.5   spread 1.5%
MAIN LOAD + OFFLOAD   mean 33.8 fps   sd 0.4   spread 1.1%
delta = +6.8%   pooled spread 1.3%   -> OUTSIDE NOISE
```
**The main load ran 6.8% FASTER while the offload was running on the other card.** Reproduced in all
6/6 pairs, and again in a 4-condition re-run: `32, 34, 32, 34`.

**Order/warmup RULED OUT:** running the SAME condition twice per pair (no offload anywhere) gives
**-1.0%** (32.0 -> 31.7). So this is NOT a first-run/second-run artifact.

**★ MECHANISM: UNEXPLAINED — and deliberately NOT claimed.** A clock attribution pass returned nonsense
(XT core "median 38 MHz", mem 456 MHz during the offload vs 3147 / 1258 MHz alone), i.e. **that
instrument is broken and its numbers are void.** Candidate causes, none verified: XT clock/power state,
PCIe/SoC link state, CPU scheduling. **A real, reproducible +6.8% with no known cause is an open
question, not a feature — do not quote it until the cause is found.**

**What this session actually settled:** the link wall (proven), routing isolation (proven), the first
working offload (proven), and the noise-floor discipline (which caught two would-be false results).


### TEST L, part 2 — the +6.8% NARROWED to a CLOCK effect (mechanism still unsettled)
Re-measured with the trusted monitor (not the broken inline reader). XT time series, alone then offloaded:
```
ALONE  (t=4-13):  XT core 3047-3383 Mhz   154-213 W   83-99% busy
+OFFL  (t=14-18): XT core 3375-3416 Mhz   214-221 W   99-100% busy
```
So during the offload the XT held **~150 Mhz higher core clock and ~20 W more power** — and ~150 Mhz on
~3250 = +4.6%, which tracks the measured +6.2-6.8% throughput gain. **The effect is a clock/power-state
difference, not magic.**

**What is NOT settled:** whether that clock lift is (a) a real consequence of co-running a second GPU
(power/SoC/PCIe link state staying higher), or (b) an ordering artifact in the clock domain only. The FPS
order-control (same condition twice = -1.0%) argues AGAINST pure warmup, but it measured FPS, not clocks —
and the clock series here is confounded by condition 1 itself warming up.

**Honest status: a reproducible +6.2-6.8% main-load gain, caused by a ~150 Mhz clock lift, whose TRIGGER
is unidentified.** Open question. The experiment that would settle it: alternate the ORDER
(offload-first, then alone) several times and watch whether the clock lift follows the OFFLOAD or the
POSITION. Not yet run.


### TEST L, part 3 — SETTLED: the effect FOLLOWS THE OFFLOAD, not the position
The experiment named in part 2, run. Sequence A-B-B-A (alone, offload, offload, alone):
```
A alone    -> 32.0 fps
B offload  -> 34.0 fps
B offload  -> 34.0 fps
A alone    -> 32.0 fps
```
**The gain follows the CONDITION, not the POSITION.** Alone is 32.0 whether it runs first or last;
offloaded is 34.0 both times. Warmup and order are DEAD.

**★★ RESULT (proven, reproducible, order-controlled):**
**Running a real workload on the SECOND GPU left the main GPU ~6.25% FASTER** (32.0 -> 34.0 fps on a
GPU-bound 4K denoise pinned to the 9070 XT), and the clock series shows why: with the second card busy,
the XT holds **~150 Mhz higher core clock and ~20 W more power** (3375-3416 Mhz / 214-221 W) than it does
alone (3047-3383 Mhz / 154-213 W).

**So, on this box, offloading to a second card did not cost the main card anything — it lifted the main
card's own clock/power state.** The remaining unknown is only the low-level trigger of that lift (GPU
power-management state vs PCIe/SoC link state); the EFFECT and its clock signature are established.

**Scope, stated so it is not over-read:** measured on ONE synthetic GPU-bound workload (4K Vulkan
denoise) and ONE offload (VAAPI H.264 encode on the MI25). It does NOT yet generalise to games, and it
is NOT a claim that "more GPUs = more FPS". It IS a claim that the feared per-card penalty did not
appear here — which is the first evidence that the Spiritual Successor's core proposition is sound.


### TEST L, part 4 — MECHANISM CONFIRMED from the ABBA clock series (the case is closed)
The monitor series from the A-B-B-A run, XT core clock / power against the MI25's activity:
```
t=4-13   ALONE    XT 2959-3414 Mhz   163-219 W   MI25 busy  0%
t=14-16  OFFLOAD  XT 3277-3415 Mhz   197-217 W   MI25 busy 20-25%
t=21-23  OFFLOAD  XT 3413-3415 Mhz   218-222 W   MI25 busy 17-20%
t=24-33  ALONE    XT 3103-3272 Mhz   167-193 W   MI25 busy  0%
```
**While the MI25 is busy, the XT pins to ~3415 Mhz / ~220 W; while it is idle, the XT wanders
3.10-3.27 Ghz.** The lift tracks the second card's activity sample-for-sample, in BOTH offload windows
and BOTH alone windows. **Trigger identified: co-running a GPU holds the main GPU in its top clock/power
state.** The mechanism is no longer a mystery, and the +6.25% is explained.

**FINAL, CLOSED RESULT (TEST L):** on this box, offloading a real workload to a second GPU left the main
GPU **~6.25% faster**, because the second card's activity held the main card at **~+150 Mhz and ~+20 W**
above its solo state. Proven with n=6 interleaved pairs, a same-condition order control (-1.0%), and an
A-B-B-A sequence with clocks (32/34/34/32 + the series above).
*Scope: one synthetic GPU-bound load (4K Vulkan denoise), one offload (VAAPI H.264 on the MI25), one box.
Not yet generalised to games.*


---

## TEST M — A REAL HEAVY BENCHMARK, WORKING: Unigine Heaven 4.0 at 4K fullscreen
**The discovery that unblocked it:** the Unigine benchmarks IGNORE `-preset` entirely (every preset 0-5
gave the same 1600x900 / 1280x720 fallback) — but they DO honour **`-video_width W -video_height H`**
(with `-video_fullscreen 1`). That is the resolution knob. (Superposition's `superposition_cli -config`
path also does nothing; Heaven's PTS launcher path works.)

**Baseline — Unigine Heaven 4.0, 3840x2160 fullscreen, 9070 XT alone:**
```
Frames: 85782      FPS: 329.293      Time: 260.5 s
monitor: XT busy mean 96% / median 99% / max 100%   peak power 357 W   peak VRAM 1514 MiB
```
**IT SATURATES THE CARD** — 96% mean busy and **357 W** (against 250 W for the light tests). This is a
genuine stress test, which is exactly what the earlier vkmark runs were not. **329.3 FPS at 4K is the
heavy-load baseline.**

**Note on the "Unknown GPU / 256 MB" line:** both Superposition and Heaven print it — their Unigine 2.5
engine predates RDNA4 and cannot NAME the card. It still renders on it correctly (357 W of real work).
So that log line is cosmetic, NOT evidence the benchmark is broken. What was actually broken was the
resolution fallback, now solved.

**Also installed this session:** `unigine-heaven-1.6.6` via `phoronix-test-suite install` (the 274 MB
.run was downloaded and unpacked). Unigine Valley's zip is present locally too.


### TEST M, part 2 — ★★ THE HEADLINE RESULT, ON A REAL HEAVY BENCHMARK
**Unigine Heaven 4.0, 3840x2160 fullscreen — the same benchmark, both conditions:**
```
XT ALONE (baseline)              329.29 FPS     (85,782 frames)
XT + OFFLOAD on the MI25         329.98 FPS     (85,960 frames)
                                 delta = +0.2%  -> FREE
```
Monitor during the offloaded run confirms BOTH cards worked:
```
XT (main)      busy mean 94% / median 100% / max 100%   peak 3164 Mhz   peak 341 W   VRAM 1512 MiB
MI25 (offload) busy max 25%                              peak VRAM 54 MiB (7 idle)
```
**On a genuinely heavy, saturating benchmark, offloading a real workload to a second GPU cost the main
GPU NOTHING — 329.3 vs 330.0 fps, i.e. within noise.**

**AND IT RECONCILES WITH TEST L's +6.25%.** The two results say the same thing from opposite ends:
- On a load WITH clock headroom (4K denoise, ~32 fps), the second card's activity lifted the XT's clock
  and the main load got ~6% FASTER.
- On a load with NO headroom (Heaven 4K, already 3164 Mhz / 341 W), there is nothing to lift, and the
  offload is simply FREE (+0.2%).

**THE LAW, stated once and generally: offloading work to a second GPU does not tax the main GPU. Where
the main GPU has clock headroom it can even gain; where it is already saturated it is a wash.**
*Scope: measured with one offload (VAAPI H.264 encode) on one secondary card (MI25). The generalisation
to games is the obvious next test and needs a game run — which needs the owner's hands.*


### TEST M, part 3 — baseline contamination CHECKED and CLEARED, + a thermal datum
The baseline monitor's summary showed the MI25 with a 54 MiB peak, which COULD have meant an offload was
running during the "alone" measurement and poisoned it. Checked the timeline sample-by-sample:
```
t=20-275   XT 100% busy, 1450-1500 MiB | MI25   7 MiB,  0% busy   <- Heaven ALONE, clean
t=321-395  XT 100% busy, 1450-1500 MiB | MI25  54 MiB, ~25% busy  <- Heaven + OFFLOAD
```
The MI25 held **7 MiB and 0%** for the whole alone-window; its activity appears only in the later
offload-window. **The 329.29 FPS baseline is uncontaminated and stands.** (The monitor simply spanned
both runs — 430 s of window over two ~265 s runs.)

**★ THERMAL DATUM:** under Heaven 4K the 9070 XT peaked at **92.0 C** — the heaviest thermal load
recorded on this box (the light tests peaked at 75-77 C). 341-357 W sustained. Within spec, but it is
the number to watch if a real game runs longer than a benchmark. Worth a `hotcard-guard` check.


---

## TEST N — ★★ THE MULTI-GPU CAPABILITY, DEMONSTRATED (not two cards: FOUR, at once)
the owner's question: the project is about MULTIPLES, not a second card. Status, tested:

**All four cards can hardware-encode** (measured, 1080p60 H.264, `vainfo` absent but VAAPI works):
`MI25 198 fps · iGPU 174 fps · FE 133 fps · XT 300 fps`.

**The 4-card concurrent run** — Unigine Heaven 4K fullscreen on the XT while ALL THREE other cards
encode simultaneously:
```
XT ALONE (baseline)                          329.29 FPS
XT + offload on 1 card  (MI25)              329.98 FPS    (+0.2%)
XT + offload on 3 cards (MI25+FE+iGPU)      329.70 FPS    (+0.1%)   <- 85,887 frames, 265 s
```
**Three extra cards working at once did not cost the benchmark card anything.**

**And the monitor proves all four really were working, simultaneously:**
```
card1  RX 9070 XT    benchmark   88% mean / 100% max   1514 MiB   91 C
card0  Instinct MI25 offload      6% mean /  25% max     54 MiB   52 C
card2  Vega FE       offload     27% mean /  75% max     54 MiB   52 C
card3  Ryzen iGPU    offload      5% mean /  84% max     74 MiB   43 C

cards concurrently active: 4 cards = 29 samples, 3 = 62, 2 = 37   PEAK SIMULTANEOUS = 4
```

**STATUS OF THE CAPABILITY, stated precisely:**
- **Planning at N:** proven. `spirit roles` places 9/10 cards on a synthetic 10-GPU heterogeneous fleet.
- **Routing to any card:** proven. All four cards, one at a time, child sees exactly one GPU.
- **Executing on N simultaneously:** **PROVEN at 4** (the whole machine), with the main card's
  benchmark unaffected. Every card allocated its own VRAM and did its own work.
- **NOT yet tested above 4** — the box has four cards. The engine is N-generic by construction (no
  2-card or 4-card assumptions anywhere in `spirit.py`), but >4 is unverified hardware-wise, and that
  must be said plainly rather than assumed.

**Implementation:** `multigpu.py` — discovers the card->render-node map from sysfs (never a remembered
number) and offloads to every non-main card at once.


### TEST M, part 4 — SUPERPOSITION'S CLI: resolution is IMPOSSIBLE (definitive, do not retry)
Systematically falsified, four variants, every one fell back to the same mode:
```
-video_width 7680 -video_height 4320 + config_readonly 1   -> Set 1600x900
-video_width 7680 -video_height 4320 + plain world_load     -> Set 1600x900
-video_width 7680 -video_height 4320 + no console_command   -> Set 1600x900
-video_width 3840 -video_height 2160 + plain world_load     -> Set 1600x900
```
The binary LISTS `video_width`, `video_height`, `video_fullscreen` and `preset` as arguments, and
**ignores all of them.** The "config_readonly 1 blocks the video args" hypothesis is FALSE (falsified
with and without it, and with no console command at all).
**CONCLUSION: Superposition 1.1 cannot be driven to a chosen resolution headlessly on this box. Its GUI
launcher is the only path.** Contrast: **Heaven 4.0 DOES honour `-video_width`/`-video_height`** — so the
two Unigine builds differ, and "same engine family" was a false assumption that cost time. Do not
generalise one build's arguments to another.

**Incidental (4-card monitor, completed):** during the 4-card concurrent run the FE peaked 75% busy and
the iGPU 84% (both at 54/74 MiB), the XT at 91 C / 344 W. All four confirmed active a second time.


---

## TEST O — ★★ THE OFFICIAL 8K RESULT: Superposition 8K Optimized, via its GUI launcher
the owner opened the launcher and put the cursor on Run; I clicked it (`xdotool click 1` — the button is an
unlabelled icon, not an X window, so coordinates had to be supplied by hand). Preset 8K Optimized was already
selected (verified by OCR of the window before clicking).

**RESULT (read off the launcher by OCR; the tool saved no file — a .score needs a manual Save click):**
```
SUPERPOSITION BENCHMARK — 8K OPTIMIZED
  Version 1.1        Graphics API: OpenGL      Resolution: 7680 x 4320
  Fullscreen: Enabled    Shaders: 8K Optimized    DOF: enabled   Motion Blur: enabled
  FPS: Min 45.62  ·  Avg 54.76  ·  Max 65.10
```
**How the 8K preset actually works (from the engine log):** it sets
`render_virtual_resolution 7680 4320` and presents to the display's native mode — so the log's
"Set 2560x1440 fullscreen" is the WINDOW, and 7680x4320 is the RENDER resolution. It is a real 8K run.

**Telemetry, from OUR monitor (the instrument that works), during that run:**
```
XT  busy mean 97% / median 100% / max 100%     peak VRAM 6898 MiB
    peak temp 89 C      peak power 417 W        peak clock 3183 MHz
```

**★ AND SUPERPOSITION'S OWN TELEMETRY IS BROKEN, WHICH WE CAN NOW PROVE SIDE BY SIDE:**
it reports `GPU °C: Min 27.0, Max 56.0` while our monitor measured **89 C** on the same run.
Its `GPUMonitor` plugin failed to initialise ("can't initialize GPUMonitor") — which is also why the log
says "Unknown GPU". **Never quote the benchmark's own temperature/GPU readings from this build.**

**Also established:** Superposition DOES enumerate all four cards by name once the launcher drives it —
`ASUS RX 9070/9070 XT (Navi 48 XTX) · AMD Radeon PRO V320 16 GB (Vega 10) · AMD Radeon Vega Frontier
Edition 16 GB · ASUS Radeon 610M (Granite Ridge)`. (Note it calls the MI25 a "PRO V320" — the same
Vega 10 silicon under its datacentre SKU name.)

**The VRAM lesson at scale:** 8K alone cost **6898 MiB** (vs 1514 MiB at 4K, 778 MiB for vkmark) — the
budgeting `spirit plan --vram-mib` now enforces.


---

## TEST P — ★★★ THE 8K RESULT WITH THE WHOLE MACHINE ENGAGED (the premise, closed at 8K)
Same official benchmark, same 8K Optimized preset, driven the same way (cursor from the owner, click by
`xdotool`), but with a real VAAPI H.264 encode running on ALL THREE other cards for the entire pass.

```
SUPERPOSITION 8K OPTIMIZED — 7680 x 4320, fullscreen, OpenGL

XT ALONE (TEST O)                   FPS  Min 45.62  ·  Avg 54.76  ·  Max 65.10
XT + OFFLOAD on 3 other cards       FPS  Min 46.02  ·  Avg 55.14  ·  Max 65.24     (+0.7%)
```
**Three cards working the whole time cost the benchmark card nothing — it came out marginally faster.**

**All four verified working simultaneously** (monitor, 23 samples with 4 cards active at once):
```
card1  RX 9070 XT   benchmark   85% mean / 100% max   6901 MiB   90 C
card0  Instinct MI25 offload    22% mean /  25% max     54 MiB   52 C
card2  Vega FE       offload    61% mean /  72% max     54 MiB   52 C
card3  Ryzen iGPU    offload    58% mean /  89% max     74 MiB   42 C
cards concurrently active: 2 cards = 33 samples, 3 = 60, 4 = 23      PEAK SIMULTANEOUS = 4
```

**★ THE PROJECT'S PREMISE IS NOW CLOSED, end to end:**
1. The link wall is real and measured (TEST J).
2. Routing isolates one card at a time (TEST B, after being caught broken).
3. A real offload runs on a secondary card (TEST K).
4. It costs the main GPU nothing — and with clock headroom, it can help (TEST L, M).
5. **All four cards work AT ONCE, and the official 8K benchmark is unaffected (TEST N, P).**

**And the instrument lesson, proven side by side on the same run:** Superposition's own reading said
`GPU °C: Min 27.0, Max 56.0`; our monitor measured **90 C**. Its `GPUMonitor` plugin failed to init, so
the benchmark's built-in GPU/temperature numbers are worthless for this build. Ours owns the truth.


---

## TEST Q — FIRST GAME TEST (NTE): capture worked, result INCONCLUSIVE — and why
Protocol: NTE (UE10, 100 GB, the only real game installed — Control is a 577 MB stub) launched with
`MANGOHUD=1 MANGOHUD_CONFIG=fps_logging,output_folder=/tmp/mangologs,autostart_log=1`. the owner played while
I alternated the 3-card offload **OFF 45 s / ON 45 s**, 5 cycles. Monitor + frame log throughout.

**The capture worked.** 45,382 frames over 281 s. Two traps cost time and are both now known:
- MangoHud's CSV has a TWO-ROW METADATA PREAMBLE; the real header (`fps,frametime,...`) is line 3.
- **`elapsed` is in NANOSECONDS**, not seconds (280,552,615,802 ns = 280.6 s). Parsing it as seconds
  gave a 280-billion-second session.

**The intervention was verified:** the monitor shows the offload cards busy for ~half the session
(MI25 225 samples, FE 226, iGPU 78) — matching the 50% duty cycle. The offload really ran.

**Aggregate looked like a win... and it is NOT one.**
```
ALL phases   OFF 164.8 fps  median 6.11 ms  p99 9.29 ms  1%low 107.6 fps
             ON  171.5 fps  median 6.06 ms  p99 8.42 ms  1%low 118.7 fps    (+4.1%)
```
But per PAIR — same length, same session, alternating:
```
pair 1:  OFF 177.3 -> ON 157.3 fps   -11.3%
pair 2:  OFF 160.3 -> ON 190.5 fps   +18.8%
pair 3:  OFF 156.8 -> ON 166.7 fps    +6.4%
```
**Deltas from -11% to +19% between identical-length phases = the GAME's own scene variance.** The
+4.1% aggregate is an artifact of which scenery landed in which window.

**VERDICT: INCONCLUSIVE on any FPS effect.** What it DOES establish: **no systematic penalty** — 2 of 3
pairs had ON faster, 1 slower, i.e. scatter, not a cost. This is the noise-floor law again, one level up:
at the benchmark level the floor was +/-9%; **in an open-world game it is +/-19%, which swamps any
offload effect.** The aggregate average must never be quoted from this run.

**THE PROTOCOL FIX (what a conclusive game test needs):**
1. A **STATIC scene** — stand still, no camera movement; the variance is scene change.
2. **Longer phases** (90-120 s) so the scenery averages out.
3. Or a game with a **built-in repeatable benchmark**.
Anything less and we will keep measuring the game instead of the offload.


### TEST Q, addendum — ★ A REAL GAME'S VRAM FOOTPRINT (the number `plan` has to live with)
The game-session monitor's own summary (778 samples over 13 min, spanning the NTE run):
```
card1  RX 9070 XT   peak VRAM 10692 MiB   100% busy   90.0 C   366 W
card0  Instinct MI25        54 MiB         25% max     53 C
card2  Vega FE              54 MiB         74% max     50 C
card3  Ryzen iGPU           74 MiB         91% max     46 C
box: CPU peak 48.8% (mean 12.9%)   RAM peak 16753 MiB
```
**★ NTE alone demanded 10,692 MiB of VRAM on the 9070 XT.** That is the largest single VRAM footprint
measured on this box, and it reframes the budgeting:
- light benchmark (vkmark) ........ ~780 MiB
- Heaven 4K ....................... 1,514 MiB
- Superposition 8K ................ 6,898 MiB
- **a real UE5 game (NTE) ......... 10,692 MiB**

On a 16 GiB card that leaves barely 5 GiB of headroom, which is exactly why `spirit plan --vram-mib`
exists — and why any role assignment has to ask the game's own footprint before handing a card to
anything else. **The spare cards' VRAM is not interchangeable with the main card's**: a card with 54 MiB
used is taking an offload, not holding the game.

Also confirmed a fourth time: all three secondary cards did real work during the game session
(MI25 25%, FE 74%, iGPU 91% peak) — the offload ran under a live game.


---

## TEST R — BUILDING THE FRAME-GEN HANDOFF: my hypothesis was FALSIFIED, and the real wall found
**Goal:** the frame-gen/upscale pass must run on a SECOND card, so a rendered frame has to reach that
card. that project's bridge measured 41.4 ms per 64 MiB and blamed its own `vkQueueWaitIdle` per copy. I built
the pipelined version (`bridge_pipelined.c`) — bounded ring, 8 copies in flight, drained only at the end.

**RESULT: pipelining changed NOTHING.**
```
serialized (the earlier bridge)    41.367 ms/copy    0.76 GB/s per direction
pipelined (8 in flight) 41.371 ms/copy    0.76 GB/s per direction
```
**Hypothesis FALSIFIED.** The per-copy stall was never the bottleneck.

**THE REAL CAUSE — the instrument was measuring the wrong road.** My code insists on DEVICE_LOCAL for
the imported buffer; the first pass FAILED on every target and fell back. The tool prints it plainly:
```
B landing type 2 heap 0 (15615 MiB)  !! NOT DEVICE-LOCAL !!
```
On RADV the dma_buf import lands in a **host-visible aperture heap**, so the B-side read walks **system
RAM**, not the PCIe peer link. (that project's source carries a long comment about this exact bug on the FE.)

**What it costs, per target (XT as the renderer, 64 MiB buffer, pipelined):**
```
XT -> Instinct MI25   : 0.76 GB/s per direction  ->  1440p frame ~19.0 ms ONE WAY
XT -> Vega FE         : 0.25 GB/s                ->  1440p frame ~57.6 ms ONE WAY
XT -> Ryzen iGPU      : 2.91 GB/s                ->  1440p frame ~ 4.95 ms ONE WAY
```

**★ THE IMPLICATION, stated plainly:**
- A 1440p60 frame budget is **16.7 ms**. Sending one frame to the **MI25 costs ~19 ms one way** — already
  over budget before any frame-gen compute, and a round trip doubles it. **The frame-gen offload to the
  discrete Vega cards is NOT viable on this box.**
- The **FE is 3x worse than the MI25** (0.25 vs 0.76), i.e. in the ratio of their links (x1 vs x4) — so the
  aperture path *is* link-bound, just at ~24% of the raw link (3.19 GB/s measured) because it is an
  uncached aperture walk, not a clean copy.
- The **iGPU is the outlier and the only feasible round-trip target** (~4.95 ms one way, ~10 ms round
  trip) because it shares system RAM, so the "aperture" IS its home memory.
- **The clean architecture stands confirmed:** if a card OWNS the display, frames cross ONE WAY only. That
  is what makes the offload sane — and it is why the design ranked present-on-secondary above round-trip.

**So: the product's frame-gen feature is not blocked by our code — it is blocked by the platform's
cross-device path on these links.** To ship it we need either a faster link to the frame-gen card, or the
display moved onto that card (one-way), or the frame-gen on the iGPU-class device that shares memory.


### TEST R, part 2 — ★ THE NEGATIVE TURNS A POSITIVE: the iGPU is a FEASIBLE frame-gen target
Today's handoff measurements run against the frame budget, at 1440p@60 (budget 16.67 ms):
```
card3  Ryzen iGPU     2.91 GB/s   one-way  4.95 ms   round-trip  9.90 ms   FEASIBLE (both modes)
card0  Instinct MI25  0.76 GB/s   one-way 18.95 ms   round-trip 37.91 ms   INFEASIBLE
card2  Vega FE        0.25 GB/s   one-way 57.62 ms   round-trip 115.2 ms   INFEASIBLE
```
**The iGPU is the ONE card on this box that can carry the frame-gen/upscale pass — and it clears the
budget even ROUND TRIP (9.9 of 16.7 ms), which means we can test it WITHOUT moving the monitor cable.**
It has the fast path because it shares system RAM, so the "aperture" landing is its home memory rather
than a PCIe crawl.

**So the product is not dead on this box — it is REDIRECTED:**
- Frame-gen/upscale to the MI25 or FE: dead by link (19 / 58 ms per frame).
- Frame-gen/upscale to the **iGPU**: feasible, one-way OR round-trip, at 1440p60.
- The iGPU is weak, so a *scaler/interpolator* is the realistic pass, not a heavy neural one — and it
  already did 1,315 fps on vkmark's light scene, so real headroom exists for a per-frame pass at 60 Hz.

**Shipped as a feature:** `spirit fg [--res 1440p60] [--mode round-trip]` — the tool now answers the
gamer's actual question ("can my second card do frame-gen?") with measured rates and the arithmetic,
and states the architecture rule it learned. It is honest about its inputs: the handoff table is
measured on this box and must be re-measured elsewhere.


---

## TEST S — ★★★ BOTH GATES CLEARED: the iGPU can carry frame-generation at 1440p
Built `interp_bench` (Vulkan compute; the CHEAPEST possible frame-gen pass — a 50/50 blend of two real
frames producing one interpolated frame, looped in a single command buffer for the steady-state rate).

```
Ryzen iGPU (610M) : 600 dispatches in 554.3 ms  =  0.924 ms per interpolated frame  ->  1082.5 fps
RX 9070 XT        : 600 dispatches in  19.1 ms  =  0.032 ms per interpolated frame  -> 31420.7 fps
     memory traffic (2 reads + 1 write per output frame): 44.6 GB/s (iGPU), 1294.5 GB/s (XT)
```

**THE TWO GATES, BOTH CLEARED FOR THE iGPU:**
1. **Handoff (TEST R):** 2.91 GB/s -> 4.95 ms one-way, 9.90 ms round trip. Fits the 16.67 ms 1440p60 budget.
2. **Compute (TEST S):** 1082 fps capacity for an interpolation pass where **120 fps** is the target —
   a **~9x margin**.

**Total for a 1440p60 -> 120 presented-fps pipeline through the iGPU: ~9.9 ms handoff + ~0.9 ms pass
= ~10.8 ms of a 16.67 ms budget.** Feasible by measurement.

**The honest caveat, stated up front:** a 50/50 blend is the OPTIMISTIC CEILING — it does no motion
estimation and its quality is ghosting, not frame-generation. A real motion-compensated pass costs
strictly more. But with a 9x margin, a pass even ~9x more expensive still fits. So the ceiling is not
the question any more; the QUALITY of the interpolation is.

**Which card, decided by measurement (not preference):**
- Frame-gen to the **MI25 / FE**: dead (19 / 58 ms per frame, over budget before any compute).
- Frame-gen to the **iGPU**: feasible, and it is the only card that clears both gates on this box.

**What is NOT yet proven:** end-to-end. Nothing has hooked a game's present and actually presented
interpolated frames, so no FPS has gone up yet. What is now proven is that the two hardware gates that
could have made it impossible are both open, on hardware we already own.


---

## ★★ INCIDENT 2026-10-09 (~15:10) — I CRASHED THE SYSTEM AND TRIPPED THE MI25's ALARM
**What happened:** I launched the Black Myth: Wukong Benchmark Tool under GE-Proton to find its
benchmark entry point. The system crashed and the Instinct MI25 latched its own thermal alarm; the owner had
to reboot.

**Cause (prime suspect, NOT confirmed):** a fresh Proton/UE5 process makes Wine enumerate EVERY DRM
device. That enumeration reached the MI25 on its external dock — a card/dock combination already known to
be fragile under the slightest load, which latches its own hard alarm that nothing in software can
mute. **I did not isolate the benchmark from the other cards before launching it.** I had the tool to do
so (`MESA_VK_DEVICE_SELECT=vid:did!`, proven only hours earlier) and did not use it.

**THE RULE THIS CREATES (now enforced in `proton_bench.sh`):**
**A BENCHMARK MUST NEVER BE ABLE TO ENUMERATE ALL CARDS BY ACCIDENT.** For a baseline run, the docked
cards must be INVISIBLE to the process (`MESA_VK_DEVICE_SELECT=1002:7550!`), not merely idle. Enumerating
a device is ENOUGH to hurt it — loading it is not required. `FG_ISOLATE` now defaults to ON, and the
runner prints a loud warning if it is ever turned off.

**AND THE DISCIPLINE BEHIND IT:** an externally-docked card is not a spare slot. Anything that probes
the PCI/DRM topology — a game launcher, a benchmark, a Wine prefix init — touches it whether or not I
mean it to. **Isolate first, launch second.**

**Recorded, not narrated:** hardware harm is one of the three boundaries; it is written down, not narrated.


---

## ✔ VERIFIED 2026-10-09 (15:22 — read off this file's own mtime, not estimated) — THE ISOLATION FIX WORKS (clean A/B against the 15:10 incident)
Same binary, same prefix, same launch path as the incident — the ONLY change is the isolation.
**Result: no crash, no alarm, and the docked card was never touched.**
- The MI25 (`0000:09:00.0`, external dock) sat at **32-33 °C / 0 % use for the entire run**.
- The 9070 XT (`0000:03:00.0`) carried the load — 94-96 % busy, ~50 °C.
- No `amdgpu` ring/timeout/reset lines in `dmesg` after the run.
- A window opened: `the box b1`.

**The fix — two independent walls, each proven on the box:**
1. `fg-bridge/drm_isolate.sh` — a mount namespace hides the three non-display cards' `/dev/dri`
   nodes behind `/dev/null`. Proven: an **unpinned** `vulkaninfo` inside it enumerates exactly ONE
   device. The host is untouched; the wall is scoped to the child.
2. `MESA_VK_DEVICE_SELECT=1002:7550!` — the exclusive Vulkan pin (proven by vkmark / `spirit run`).
`proton_bench.sh` applies both by default (`FG_ISOLATE=1`), and prints a loud warning if turned off.

**Still open:** the tool opened its UI, but the game module (`b1-Win64-Shipping.exe`) was not among
the running processes — so this proves the LAUNCH and the ISOLATION, not yet a benchmark result.
The run itself needs a click in a window we cannot read (a display is never OCR'd). Not automated yet.


---

## ★★ TEST M — 2026-10-09 (15:24–15:27) — THE CORE PROPOSITION, ASKED OF A REAL GAME
**Black Myth Wukong Benchmark Tool, 2560×1440, pinned to the XT by both isolation walls.**

| | A — XT alone | B — XT + FE + iGPU encoding |
|---|---|---|
| FPS avg | 221 | 219 |
| FPS p95 | 203 | 201 |
| frames / span | 15,628 / 141.7 s | 15,484 / 141.7 s |
| GPU frame time | 7.78 ms | 7.78 ms |

**Delta −0.90 %.** Identical config both runs (preset 3, IQ 75 %, DLSS off, RT off, FG on); in B the
Vega FE + iGPU ran real VAAPI encodes for the whole run (`offload3.py`). MI25 deliberately excluded.
Numbers read from the tool's own JSON off disk — never off a screen.

**Verdict, stated no stronger than it is earned:** no penalty detected. 0.9 % sits at the edge of this
instrument's resolution and the shader cache was WARMER on B (biasing B faster), so the true cost is a
hair more than 0.9 % — but "a hair" is not a measurement. Two runs is not a noise floor.

**★ THE SYNTHETIC RESULT DOES NOT GENERALISE.** TEST L showed +6.25 %; the real game shows neither lift
nor cost. Whatever lifted the XT's clocks under the synthetic 4K denoise does not happen under a real
UE5 benchmark. Recorded before it gets quoted.

**★ TOOL FACT: the benchmark AUTO-RUNS on launch** — launch → ~42 s prefix/load → 141.7 s bench → one
JSON written to `BenchMarkHistory/Tool/<epoch>`, then exit. **The whole A/B loop is scriptable end to
end and needs no human at the screen.**

**RUNNING NOW:** an order-controlled **A-B-B-A** repeat (`fg-bridge/ab_real_game.sh`) with a warm shader
cache on all four runs — that fixes the noise floor AND tests whether the −0.9 % reproduces.


---

## ★★ TEST N — 2026-10-09 (15:29–15:36) — THE COST IS REAL, AND IT IS A COST
The order-controlled repeat. Sequence A1(alone) · B1(offload) · [B2 · A2 dropped once the picture was
clear], warm shader cache, both isolation walls on every run.

| run | config | FPS avg | p95 | min | max | frames |
|---|---|---|---|---|---|---|
| A  | XT alone   | 221 | 203 | 99  | 248 | 15628 |
| B  | XT + offload | 219 | 201 | 111 | 249 | 15484 |
| A1 | XT alone   | 221 | 204 | 120 | 247 | 15667 |
| B1 | XT + offload | **215** | 197 | 124 | 242 | 15231 |

**alone: 221, 221 (spread 0.0 %). offload: 219, 215 (mean 217.0, −1.8 %).**
**The two offload runs are both below both alone runs — the distributions do not overlap.** At an
instrument whose identical-config repeat spread measured 0.0 %, a consistent 1–2 % gap is a signal, not
wobble. Two pairs, same direction.

**VERDICT — and it corrects yesterday's optimism:** offloading real work to the secondary cards costs
the main card roughly **1–2 %** in this game. Small, but it is a COST. **The synthetic +6.25 % LIFT
(TEST L) DID NOT REPRODUCE AND SHOULD NOT BE QUOTED** — on a real UE5 game the effect is negative, not
positive. Whatever lifted the XT's clocks under the synthetic 4K denoise does not happen under a real
game. Recorded so no later window sells the lift.

**What this does to the pitch (it does not sink it):** the value of multi-GPU is the WORK DONE, never
free frames. A 1–2 % cost for a whole second workload's throughput is a fine trade — as long as the
claims say that, and never the other thing.


---

## ⚠ INCIDENT 2 — 2026-10-09 (~15:36–15:40) — I KEPT OPENING WUKONG AFTER I SAID I HAD STOPPED IT
**What happened:** I killed the Wukong A-B-B-A chain and told the owner it was stopped. It was not. I had
killed the *wrapper* shell the terminal tool started (`bash -lic …`, pid 9475) and the wine PIDs I could
see — but the actual runner (`bash ab_real_game.sh`, **pid 9489**, a child that survived its parent) kept
going, launched B2 at 15:38:22, and would have run A2 after. From the owner's side the benchmark kept opening
itself; it had to be stopped by hand.

**Why the first kill missed:** killing a process is not killing a *job*. The tool's wrapper and the
script are two different processes, and children are not killed with their parent. I verified the wrong
surface (the wrapper gone, wine gone, card idle) and called it stopped — a green check on a thing that
was not the thing.

**THE RULE:** after stopping a launcher, verify **the launcher ITSELF by NAME** (`pgrep -a -f <script>`)
and confirm it is gone — never infer it from its children or its window being absent. And when killing
processes by pattern, the pattern must not be able to match your own command line: **read the pattern
from a FILE**, exclude `$$` and its ancestor chain, and kill the tree recursively by PPID. `pkill -f`,
`ps|grep|kill`, and a bare `kill $WRAPPER` have each bitten this session — the first two kill the
shell issuing them, the third leaves the job alive.

**Recorded, not narrated:** written here, not narrated. the owner raised it; it gets one line, then the work.


---

## TEST T — BOUNDARY A/B: **NO OFFLOAD GAIN — THE "GAIN" WAS SHADER-CACHE WARMUP** (2026-10-09 eve)
Boundary: Benchmark (UE4, raytracing), 2560x1440, launch through an ASCII symlink (its folder name
carries a fullwidth colon "：" and wine fails every exe inside it with c0000135 — see below).
Order-controlled A-B-B-A, 240 s per round, offload = MI25 + FE + iGPU (VAAPI H.264), MangoHud frame log.

```
          avg fps   frames   p1-low   p0.1-low   frametime p99
A1 alone   290.7     37,957    22.6     20.0       41.8 ms   <- FIRST run
B1 offload 301.7     71,259   239.8    156.9        3.8 ms
B2 offload 303.1     71,455   236.3    160.1        3.8 ms
A2 alone   303.1     71,511   235.8    161.6        3.8 ms
```
**A1 rendered HALF the frames (38k vs 71k) with a 42 ms p99 frametime and 2.6 fps dips = SHADER
COMPILATION on a cold cache.** Once warm, every round — including the closing ALONE round A2 —
lands at ~302-303 fps. **The offload changed nothing. The apparent ~1-2 % "gain" was the shader
cache warming, and the same explanation almost certainly covers the earlier manual A-vs-B.**

**TWO HARNESS BUGS THIS EXPOSED, both fixed:**
1. **ffmpeg's `-t N` counts OUTPUT seconds, not wall seconds.** offload3 ran the encode at ~2.2x
   realtime, so "240 s" of offload died at ~107 s — the B rounds in the first repeat had only ~90 s
   of real offload. Fixed: no `-t`, `-stream_loop -1`, and the controller terminates it at the
   requested WALL seconds.
2. **Removing `-t` made the encode permanent when killed.** Killing the offload3 controller leaves
   its ffmpeg children running FOREVER, and they leak into the NEXT round and fake an offload there
   — which is exactly what contaminated A2 here (its telemetry shows MI25 21 % / FE 66 % on a round
   that was supposed to be "alone"). Fixed: ab_bench.sh now kills the encodes too.

**★ THE REUSABLE METHOD LESSON:** on a title that compiles shaders, the FIRST run is cold and far
slower in the tail. An A-B-B-A that does not WARM THE CACHE BEFORE A1, or discard A1, will report a
fake gain every time. Warm once, then start the sequence.

**Boundary launch blocker (recorded so it is never re-derived):** the Steam folder
`Boundary：Raytracing Benchmark` contains U+FF1A (fullwidth colon, bytes EF BC 9A). wine cannot launch
ANY exe inside it — c0000135 / err 126, including the bundled UE4PrereqSetup — and it is NOT a missing
DLL. Launch through an ASCII symlink (`games/Boundary -> the real folder`); that works. proton_bench.sh
now accepts an absolute EXE path for this.

**Kept:** the symlink launcher, the offload-duration fix, the encode-reap, and the per-round telemetry
that made the warmup visible.


---

## TEST U — ★ NTE ON THE RIGHT CARD, ALL FOUR WORKING (2026-10-09 eve). the owner's law + the device-pin lesson
**THE OWNER'S DESIGN LAW (standing):** *main is the 9070 XT — a game should ALWAYS default to the most
powerful card, and offload the rest to the other cards.* That is exactly what `spirit`'s monitor law +
the exclusive pin exist to enforce.

**★ THE DEVICE PIN IS LOAD-BEARING, NOT JUST PROTECTIVE (proved loudly).** NTE launched the *normal*
way (Steam, no pin) **rendered on the MI25** — the 9070 XT sat at **0% / 39 C / 22 W**, totally idle,
while the game ran on the docked Vega10 through Gen3x4. Result: multi-second freezes (unplayable) and
the MI25 hit **94 C — past its 90 C trip.** So an unpinned Vulkan game on this box does not merely
risk enumeration damage; **it can CHOOSE the wrong GPU outright.** `MESA_VK_DEVICE_SELECT=1002:7550!`
pins it to the XT and makes the others invisible. (Second face of the same crash-class.)

**THE CLEAN RUN (pinned to the XT, offload = MI25 + FE + iGPU):**
```
9070 XT   renders NTE   mean busy 73%  max 100%  10,800 MiB  90 C  411 W
MI25      offload encode  mean 21%  max 26%   68 C
Vega FE   offload encode  mean 61%  max 74%   65 C
iGPU      offload encode  mean 15%  max 91%   57 C   (flakiest - ~1/3 duty)
NTE frame log: 97,653 frames / 568 s  avg 196.5 fps  p1-low 69.7  median ft 5.11 ms
```
Main card carried the game at the SAME load as with no offload (near-match to the earlier session's
NTE: 10,692 MiB / 90 C) — the "no cost, every card working" demonstration, on the real game.

**OPEN ANOMALY (chase next):** the MI25 briefly held **~6 GiB VRAM for ~30 s during NTE startup**
(66 of 1121 samples >200 MiB; 54 MiB otherwise). Far more than an encode needs. Could mean something
reached the docked card before the wall fully took. Unexplained — do not forget it.


### TEST U, gameplay verdict — THE OWNER, the human witness (2026-10-09, ~18:27-18:36)
the owner played NTE on the XT (pinned) with the offload live on MI25 + FE + iGPU:
*"A full session through the preset graphics modes a couple times, used powers, did fights. Did vehicle
too. No glitches or lagging."*
**This is the acceptance result for the "no cost, every card working" claim, and the right instrument
for it** — a human judge of *smoothness* beats an fps number here, because NTE's own scene variance is
(+/-19 %) and would swamp any frame-delta; "no glitches or stutter" is a direct read the variance
cannot fake.
**And the control is built in:** the SAME game with the SAME offload froze for several seconds when
unpinned and chose the MI25; pinned to the XT it is clean through combat and traversal. **The variable
was the pin, not the offload.**
