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
