# Spiritual Successor

**Your game gets one GPU. Every other GPU gets a job.**

Your game runs on the card your monitor is plugged into. Every other card in the machine does real work
at the same time — a video encode, a compute job. Quit the game and they stop.

Two cards, four cards, ten cards, or just the graphics built into your processor sitting next to a real
card. Different makers, different ages, different speeds. That is the normal case, not a complication.

Linux. No root. No dependencies.

**Three implementations of the same tool.** Python (`spirit.py`), C++ (`cpp/`, plain `make`), and Rust
(`rust/`, `cargo build --release`). All three are held to byte-identical output — not claimed, tested.

## What this is not

- **Not SLI and not CrossFire.** Those tried to make several cards behave like one faster card. This
  does not do that.
- **Lossless Scaling is not included, in any form.** We do not ship it and we do not copy it. If you
  happen to own it, this tool can drive it — see "More frames" below.
- **It does not raise your frame rate.** Not yet. The spare cards do not add frames to your game.
- **It does not turn a mid-range card into a 5090.** Nothing does.
- **Not Nvidia.** This is Linux, and the pinning uses Mesa, so AMD and Intel work today. Nvidia's driver
  does not give us the same control.

## What it does

```
spirit play -- ./yourgame
```

1. Finds the card your monitor is plugged into.
2. Launches the game so it can only see that card.
3. Starts a real video encode on every other card, so they work while you play.

## The trap this closes

Leave a game alone and it picks a card for itself — and it can pick the wrong one. We watched it happen:
a game with no pinning picked a slow card on an external dock instead of the fast one. It ran at a crawl,
it got that slow card hot, and the fast card sat idle the whole time.

Your game should use the card it should, and only that card. That is the fix.

## Install

```
curl -fsSL https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main/install.sh | sh
spirit doctor
```

`doctor` does not describe your machine — it tests it. It launches a real program on each card and checks
that it can see it, read it, and pin work to it. If something is wrong, it says so.

If you would rather do it by hand: download `spirit.py`, put it anywhere on your PATH, run it.

## Commands

```
spirit doctor        test that it really works on your machine — run this first
spirit probe         list your cards, and how fast each can actually talk
spirit roles         give every card a job — or say plainly why it cannot have one
spirit play -- CMD   run a game, with your other cards working underneath
spirit plan --workload ...                             can this job run on this card?
spirit boost         what extra frames this machine can give you, right now
spirit fg --res 1440p60                               can a spare card carry frame generation?
spirit run --role encode --card card2 -- ffmpeg ...   hand one job to one card
```

## More frames: two ways

**1. Upscaling — render below native and scale up.** Fewer pixels rendered is real, immediate frames.

```
spirit play --scale 0.7 -- ./yourgame
```

That renders at 70% of your monitor's resolution and scales it back up with FSR 1.0, through gamescope.
On a 2560x1440 display that means rendering 1792x1008 — 49% of the pixels. It works on any Vulkan game,
with no per-game setup.

**2. Frame generation — extra frames, interpolated.** The bigger multiplier, and it needs software this
tool does not ship: `lsfg-vk`, which requires owning Lossless Scaling. If you have both, the tool drives it:

```
spirit play --framegen 2 -- ./yourgame
```

Two honest notes. **Run `spirit boost` first** — it reports what is and is not available on your machine,
and says exactly what is missing when something is. And frame generation invents frames, so it can look
wrong on fast movement; that is the technology, not this tool. If it looks off, turn it off.

Combined: `spirit play --scale 0.7 --framegen 2 -- ./yourgame`

## Questions from the group post

**Do the cards have to match?** No. An RX 580 next to a Vega 7, a new card next to the one built into
your CPU — the tool asks what each card can do and gives it that.

**Is this SLI or CrossFire?** No.

**Is this Lossless Scaling?** No. That scales a frame; this moves work onto other cards.

**Will it work on my laptop, integrated graphics plus a dedicated card?** Very likely. Run
`spirit doctor` — it answers for your machine instead of guessing.

**What about my 2080 with my 4070?** Not today. That is Nvidia, and the pinning is Mesa based. AMD and
Intel work now.

**Will my game stutter?** Handing work to the other cards cost the main card about 1–2% in one game and
nothing measurable in another. In a full open-world session the player reported no stutter.

## What is true, and what is not

Everything here was measured on our own machine. Where we could not measure it, we say so.

- **Your frame rate does not improve today.** The win is the work the spare cards do, not extra frames.
- **Frame generation is available — but it is not ours and we do not ship it.** It comes from `lsfg-vk`,
  which requires you to own Lossless Scaling. `spirit boost` tells you whether it is available on your
  machine. We do not bundle it, and we do not reimplement it.
- **Upscaling is ours and it is tested.** `--scale` is a real FPS gain, measured on a real game, and it
  costs your game nothing but the resolution you chose to give up.
- **Two things fooled us, and they are here so they do not fool you.** We once thought a busy second card
  made the main card *faster* — it looked real, and a real game killed it. And the first run of any game is
  slow while it builds its shader cache, which looks exactly like a gain. Warm up before you compare.

## Who this is for

Anyone with more than one graphics chip and a budget. If you have ever wanted a cheap second card to do
something instead of sitting there, this is for you.

## Bugs, ideas, and disagreements

Email **sophia.wanders@proton.me**. Tell me your cards, the game or the job, what you expected, and what
actually happened. A wall of detail beats a one-line report.

---

<details>
<summary>For the technical reader — what is underneath</summary>

- **Links, not specs.** `lspci` and friends report the link between a GPU and the chip it sits behind, not
  the link to the CPU. A card on a dock or a chipset slot can read `x16` while its real host link is `x1`.
  `spirit probe` reports the **minimum negotiated width along the sysfs chain**, which is the truth. On the
  dev box: RX 9070 XT `x16 Gen5` (54.9 GB/s), Instinct MI25 `x4 Gen3` (3.19), Vega FE `x1 Gen3` (0.87).
- **Never split the frame.** A 1440p frame is 14.7 MB; at 60 fps an `x1` card needs 17 ms to move it, which
  is more than the whole frame budget. Per-pixel, per-frame work across these links is dead by arithmetic.
  Cross the wire once, or at frame rate — never per pixel.
- **The pin is load-bearing.** `MESA_VK_DEVICE_SELECT=<vid:did>!` — the trailing `!` is the exclusive flag.
  Without it the loader only *reorders* and a child still sees every GPU; with it, a child sees exactly one.
  Measured: an unpinned UE5 game rendered on the docked MI25 while the main card sat at 0% busy.
- **Two walls.** The exclusive pin, plus a mount namespace that hides the other cards' `/dev/dri` nodes from
  the game. `spirit doctor` proves both, per card, on your machine.
- **No 2-card or 4-card assumptions.** `spirit roles` placed 9 of 10 cards on a made-up 10-card machine; the
  tenth had nowhere to plug a monitor, and the tool says so.

Raw measurement logs sit beside this file.

</details>

Copyright 2026 Sophia Marie DeClue. MIT.
