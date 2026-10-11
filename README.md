# Spiritual Successor

**Your game gets one card. Your other cards get a job.**

Your game runs on the card your monitor is plugged into. Every other card in the machine can be doing
real work at the same time — encoding video, on its own chip:

```
spirit play --work lecture.mp4 -- ./yourgame
```

That is a real job with a real file at the end of it. Measured on the test machine: three mismatched
cards, two of them from 2017, transcoded 240 seconds of 1080p while the gaming card sat at **0–2% and
25 W**. Three files came out.

Different makers, different ages, different speeds. Two cards, four cards, or the graphics built into
your processor sitting next to a real card. That is the normal case, not a complication.

Linux. No root. No packages to install — it is one Python file. It drives tools your machine may already
have: ffmpeg for the jobs, gamescope for upscaling, `unshare` for the second wall. It says so when one is
missing rather than failing quietly. The optional frame-gen witness is the only piece that wants root,
and you do not have to install it.

Three builds of the same tool: Python, C++, Rust. All three produce identical output — tested, not
claimed.

---

## What this is not

- **Not Lossless Scaling.** That scales one frame up and invents frames between them. This tool does
  neither — it moves work *off your game's card* onto the others. If you own Lossless Scaling, this
  tool can *drive* it (see **More frames**), but it does not include it, copy it, or reimplement it.
- **Not SLI or CrossFire.** Those tried to make several cards behave like one faster card. They needed
  matched cards, a bridge, and games rewritten to support them. This needs none of that.
- **Not PRIME, DRI_PRIME, or "multi-GPU".** Every one of those moves your game's *rendering* to the big
  card so that a smaller card can run the display. This is the opposite: your game keeps the card it
  should have, and the other cards go and do something else entirely.
- **Not a way to make your game faster.** Your spare cards do not add frames to it. The win is the work
  they do while you play.
- **Not Nvidia.** The pinning is Mesa-based, and the jobs run on a card's own video encoder — today
  that means AMD (`radeonsi`) or Intel (`iHD`). An Nvidia card can be the card your game runs on, but it
  cannot take the offload, and the tool names it as skipped rather than leaving it idle and saying
  nothing.

---

## What it does

```
spirit play -- ./yourgame                        # your game, on the right card
spirit play --work lecture.mp4 -- ./yourgame     # plus a real job on the others
```

1. Finds the card your monitor is plugged into.
2. Launches the game so that it can only see that card.
3. Runs a job on every other card — a real transcode on that card's own encoder, written to a real file.

Without `--work`, the other cards run a test load instead. That proves they can be driven. It produces
nothing, and the tool says so rather than letting you assume otherwise.

## How you can see it happened

When your game exits, the tool prints what each other card actually did. A divided job, three cards,
300 seconds each:

```
  ── JOB OUTPUT ────────────────────────────────────────────────────────
  renderD131     lecture.renderD131_part1.mp4  (286.0 MB, 300.0s)
  renderD129     lecture.renderD129_part2.mp4  (279.0 MB, 300.0s)
  renderD130     lecture.renderD130_part3.mp4  (279.0 MB, 300.0s)
  ── SPLIT CHECK ───────────────────────────────────────────────────────
  slices written      : 3 of 3
  slice durations sum : 900.0s
  original duration   : 900.0s
  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1%).
  ── THE TAIL — what the other cards did ───────────────────────────────
  renderD131   Ryzen iGPU             REAL JOB  .../lecture.renderD131_part1.mp4
  renderD129   Instinct MI25          REAL JOB  .../lecture.renderD129_part2.mp4
  renderD130   Vega Frontier Edition  REAL JOB  .../lecture.renderD130_part3.mp4
```

Every output is checked on disk afterwards — it exists, and its duration adds up. A slice that did not
get written says `MISSING` and the split says `INCOMPLETE`. Nothing here takes a process's exit code
for an answer.

A card the tool cannot drive is named rather than quietly left out:

```
  spare: SKIPPED renderD134 (Tesla P40) — no VAAPI encoder this tool knows of (kernel driver: nvidia)
```

If you use frame generation, the tool also prints the one number frame generation actually moves — and
it measures that itself, from *below* the frame-gen layer, with a small Vulkan layer it ships
(`present-counter/`). No game counter and no overlay can see those frames. On the test machine the same
300 frames of work produced **300** presented images with frame generation off and **1200** with it on
at 4x.

Your game's own counter is a different thing and it cannot see them: it counts the frames the game
rendered, and with frame generation on the game may be doing less work, because it is now paying for the
interpolation. On one title here that figure fell about 9%; on another it did not move at all. Either
way it cannot see what was added, which is why the tool carries its own witness.

What the witness does not tell you is whether the invented frames *look right*. No counter can. That one
you look at.

## Install

```
curl -fsSL https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main/install.sh | sh
spirit doctor
```

`doctor` does not describe your machine — it tests it. It launches a real program on every card and
checks that the card can be seen, read, and pinned. Give it a minute.

There is also a small local page if you would rather not use a terminal:

```
spirit ui
```

It shows your cards, what each one is for, whether upscaling and frame generation are available here,
and a box to launch your game from. It listens on your own machine only.

## Commands

```
spirit doctor        test that it really works on your machine — run this first
spirit probe         list your cards, and how fast each can actually talk
spirit roles         give every card a job — or say plainly why it cannot have one
spirit play -- CMD   run a game, with your other cards working underneath
spirit boost         what extra frames this machine can give you, right now
spirit ui            the same thing as a local page, if you would rather click than type
```

## More frames — two things people mix up

**Upscaling** renders fewer pixels and scales the picture back up. Free frames, on any card, in any
Vulkan game.

```
spirit play --scale 0.7 -- ./yourgame
```

On a 2560x1440 screen that renders 1792x1008 — 49% of the pixels. It runs through gamescope, which you
need installed. Handing work to the other cards cost the game about 1–2% in one title and nothing
measurable in another.

**Frame generation** invents frames between the real ones. It needs `lsfg-vk`, which needs you to own
Lossless Scaling. If you have both, the tool drives it:

```
spirit play --framegen 2 -- ./yourgame
```

Run `spirit boost` first: it tells you what is and is not available on your machine, and says what is
missing when something is. Frame generation can look wrong on fast movement — that is the technology,
not this tool.

## Who this is for

Anyone with more than one graphics chip. If you have an old card in a drawer and a slot to put it in,
this turns it back into something useful instead of a paperweight. Same for the graphics built into
your CPU, which sits idle the whole time you play.

## Telling me it went wrong

Email **sophia.wanders@proton.me**. Tell me your cards, the game or the job, what you expected and what
actually happened. A wall of detail beats a one-line report.

---

<details>
<summary>For the technical reader — what is underneath</summary>

- **Links, not specs.** `lspci` and friends report the link between a GPU and the chip it sits behind,
  not the link to the CPU. A card on a dock or a chipset slot can read `x16` while its real host link is
  `x1`. `spirit probe` reports the **minimum negotiated width along the sysfs chain**, which is the
  truth. On the dev box: RX 9070 XT `x16 Gen5` (54.9 GB/s), Instinct MI25 `x4 Gen3` (3.19), Vega FE
  `x1 Gen3` (0.87).
- **Never split the frame.** A 1440p frame is 14.7 MB; at 60 fps an `x1` card needs 17 ms to move one,
  which is more than the whole frame budget. Per-pixel, per-frame work across these links is dead by
  arithmetic. Cross the wire once per job, or at frame rate — never per pixel.
- **The pin is load-bearing.** `MESA_VK_DEVICE_SELECT=<vid:did>!` — the trailing `!` is the exclusive
  flag. Without it the loader only *reorders* and a child still sees every GPU; with it, a child sees
  exactly one. Measured: an unpinned UE5 game rendered on the docked card while the main card sat at 0%.
- **Two walls.** The exclusive pin, plus a mount namespace that hides the other cards' `/dev/dri` nodes
  from the game. `spirit doctor` proves both, per card, on your machine. Proton titles skip the
  namespace, because Wine refuses to run as root.
- **No 2-card or 4-card assumptions.** `spirit roles` placed 9 of 10 cards on a made-up 10-card machine;
  the tenth had nowhere to plug a monitor, and the tool said so.

</details>

Copyright 2026 Sophia Marie DeClue. MIT.
