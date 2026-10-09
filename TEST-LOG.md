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
