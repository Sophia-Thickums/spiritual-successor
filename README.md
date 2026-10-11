# Spiritual Successor

**Your game gets one card. Your other cards get a job — when you give them one.**

Your game runs on the card your monitor is plugged into. Your other cards can be doing real work at the
same time — encoding video, on their own hardware encoders:

```
spirit play --work video.mp4 --split -- ./yourgame
```

That cuts the video into pieces and encodes one piece on each card it will use. When the game exits, it
checks on disk that the pieces add back up to the full length of the original.

**By default it uses only cards on a wide host link.** A card behind a narrow slot or a slow riser is
named and left out unless you ask for it:

```
  job: NOT using renderD129 (Vega 10) — the host link is narrower than the card reports (x4)
       Add --spare renderD129 to include it anyway.
```

Verified on AMD cards only. Intel is written but has not been run. An Nvidia card can be the card your
game runs on, but it cannot take the job.

**Give no `--work` and your other cards stay idle.** No work is run on them. If you want to see them
driven first, `spirit play --test-load 30 -- ./yourgame` starts a bounded test on each one and stops on
its own. `spirit doctor` only tests the machine; it starts no load.

Cards of different makers and ages sitting in one machine is the normal case. The tool reads what each
card is — its driver, its link, whether it can encode at all — and gives it work it can take. It does not
interrogate the card's encoder capabilities; that is honest scope, not a claim. What is *verified* so far is narrower than what is written: the
encoder path has only ever been run on AMD hardware.

Linux. No root. No packages to install — it is two Python files (`spirit.py` and the local page). It
drives tools your machine may already have: ffmpeg for the jobs, gamescope for upscaling, `unshare` for
the second wall, and `vulkaninfo` if you want `doctor` to be able to test device routing. It says so when
one is missing rather than failing quietly, where it can. The optional frame-gen witness is the only piece that wants
root, and you do not have to install it.

Three builds of the same tool: Python, C++, Rust. All three are held to byte-identical output by two
differential test suites that compare each against the reference, on a fake fleet and on the live box.

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
  nothing. Intel's path is written but has never been run — see the technical notes below.

---

## What it does

```
spirit play -- ./yourgame                        # your game, on the right card
spirit play --work lecture.mp4 -- ./yourgame     # plus a real job on the others
```

1. Finds the card your monitor is plugged into.
2. Launches the game so that it can only see that card.
3. Runs a job on your other cards — a real transcode on each card's own encoder, written to real files.

**Give no `--work` and nothing is run on them.** They are left alone, and the tool says so both before
launching and when the game exits. A card the tool cannot drive is named rather than silently passed over.

What does a job cost the game? Our own runs found a few percent at most in one title and no measurable
difference in another — but each side of those comparisons was only two runs, so the honest reading is
*small or none*, not a percentage. Measure it on your own machine.

### Playing everything, Steam included, without thinking about it

Run this once a session:

```
spirit enable
```

It arms your session: every app you start from then on inherits the pin, so a game Steam launches when
you click Play is pinned with no launch options to edit and this tool nowhere in the loop. **It is
deliberately session-only** — nothing is written for boot, so a reboot always comes back with nothing
armed. When you trust it, `spirit enable --persist` writes it for future logins as well; `spirit disable`
undoes either.

Two limits, stated because they are real: a Steam that was **already running** when you armed it did not
pick the pin up — its environment was fixed when it started, so restart Steam. And arming hides your
other cards from Vulkan apps in the session, which is the point for a game and the reason it is not on
by default.

If you would rather pin a single game by hand, put the tool *inside* Steam's launch line instead:
right-click the game → Properties → Launch Options, and paste

```
"/full/path/to/spirit.py" play -- %command%
```

Note the `--` before `%command%` — the game's own command line goes after it, untouched.

## How you can see it happened

When your game exits, the tool prints what each card actually did. Here is the shape — an example machine
with a 12-second clip and two usable spare cards:

```
  job: NOT using renderD130 (GPU D) — x1 link at the host — too narrow to use unasked
       Add --spare renderD130 to include it anyway.
  ── JOB OUTPUT ────────────────────────────────────────────────────────
  renderD131     clip.renderD131_part1.mp4  (5.4 MB, 6.0s)
  renderD129     clip.renderD129_part2.mp4  (5.4 MB, 6.0s)
  ── SPLIT CHECK ───────────────────────────────────────────────────────
  slices written      : 2 of 2
  slice durations sum : 12.0s
  original duration   : 12.0s
  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1%).
  ── THE TAIL — what the other cards did ───────────────────────────────
  renderD131   GPU C                   REAL JOB  …/clip.renderD131_part1.mp4
  renderD129   GPU A                   REAL JOB  …/clip.renderD129_part2.mp4
```

Every job output is checked on disk afterwards — it exists, it was written by *this* run, and under
`--split` its duration is added up against the input to within a second or one percent, whichever is
larger — on a clip shorter than 100 seconds the rule applied is the one-second tolerance, not the
percentage. A file that did not get written says `MISSING` and the split says `INCOMPLETE`. Nothing here
takes a process's exit code for an answer. Without `--split` each card transcodes the whole input, so a
shortened file will still report as present; the `--split` check is the one that proves the pieces
reconstruct.

(Card names are whatever `lspci` calls them, shortened — shown here as placeholders. Run `spirit probe` on
your own machine to see yours.)

A card the tool cannot drive is named rather than quietly left out, in this shape:

```
  spare: SKIPPED renderD134 (…) — no VAAPI encoder this tool knows of (kernel driver: nvidia)
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
spirit enable     # optional: arm the pin for every launch from now on, Steam included (nothing at boot)
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
spirit enable        arm the pin for every launch from now on (Steam included); nothing at boot
spirit enable --persist   same, and keep it across logins
spirit disable       disarm it, and remove the persisted pin
spirit --version     print the version and exit
spirit probe         list your cards and how each one is actually wired (the GB/s column is an estimate)
spirit roles         describe what each card is and what it could take
spirit play -- CMD   run a game on the right card; add --work FILE for a job on the others
spirit boost         what extra frames this machine can give you, right now
spirit ui            a local page showing your cards and what is available here
```

## More frames — three things people mix up

**The upscaler matters more than the frame count.** On RDNA 4, AMD's FSR 4 is an ML upscaler built for
that silicon, and it costs on the order of 1–2 ms for a large drop in render cost. In a Proton game it is
one flag:

```
PROTON_FSR4_UPGRADE=1 %command%
```

(`spirit enable` sets it for the whole session.) A silent fallback to FSR 3.1 looks identical, so turn on
`PROTON_FSR4_INDICATOR=1` if you want to see which one is live. A game that already ships FSR 4 or DLSS
needs none of this.

**Upscaling through gamescope is FSR 1.0** — spatial, no motion vectors. Free frames, any card, any Vulkan
game, but the picture falls apart well before FSR 4 does. Use it for titles with no in-game upscaler:

```
spirit play --scale 0.7 -- ./yourgame
```

On a 2560x1440 screen that renders 1792x1008 — 49% of the pixels. It costs you the resolution you gave up.

**Frame generation** invents frames between the real ones. It needs `lsfg-vk`, which needs you to own
Lossless Scaling. It multiplies whatever the upscaler bought you:

```
spirit play --framegen 2 -- ./yourgame
```

Measured on this project's machine at 2560x1440: **2x costs 0.71 ms, 3x 1.18, 4x 1.66** — the tax is small,
so the multiplier should be sized so base × N lands near your panel's refresh (2x from ~80, 3x from ~55,
4x from ~41). Beyond that you are paying for frames you will never see. Run `spirit boost` first: it says
what is and is not available here. Frame generation can look wrong on fast movement — that is the
technology, not this tool.

## Who this is for

Anyone with more than one graphics chip. If you have an old card in a drawer and a slot to put it in,
this turns it back into something useful instead of a paperweight. Same for the graphics built into
your CPU, which sits idle the whole time you play.

**One honest warning about laptops.** On a laptop the built-in panel is very often wired to the
integrated graphics, not to the discrete card. The rule here is that the game renders on the card
driving your display — so on that machine it will pin your game to the *weaker* chip. On a desktop
that rule is what you want. On a laptop, run `spirit roles` first and see what it picks before you
trust it with a game.

## Telling me it went wrong

Email **sophia.wanders@proton.me**. Tell me your cards, the game or the job, what you expected and what
actually happened. A wall of detail beats a one-line report.

---

<details>
<summary>For the technical reader — what is underneath</summary>

- **Links, not specs.** `lspci` and friends report the link between a GPU and the chip it sits behind,
  not the link to the CPU. A card on a dock or a chipset slot can read `x16` while its real host link is
  `x1`. `spirit probe` reports the **minimum negotiated width along the sysfs chain**, which is the
  minimum link *width* the chain will allow — capability, not the rate it has currently negotiated; the
  negotiated one is shown separately when a card is sitting idle. The `GB/s` column beside it is an *estimate* — theoretical per-lane rate times width times a
  fixed efficiency — not a measurement; treat it as a ranking, not a speed.
- **Never split the frame.** A 1440p frame is 14.7 MB; at 60 fps an `x1` card needs 17 ms to move one,
  which is more than the whole frame budget. Per-pixel, per-frame work across these links is dead by
  arithmetic. Cross the wire once per job, or at frame rate — never per pixel.
- **The pin is load-bearing.** `MESA_VK_DEVICE_SELECT=<vid:did>!` — the trailing `!` is the exclusive
  flag. Without it the loader only *reorders* and a child still sees every GPU; with it, a child sees
  exactly one. Measured: an unpinned UE5 game rendered on a card it should not have, while the card
  driving the display sat at 0%.
- **Two walls.** The exclusive pin, plus a mount namespace that hides the other cards' `/dev/dri` nodes
  from the game. `spirit doctor` reports both: it tests the pin per card, and it says plainly whether
  unprivileged namespaces are available on your system. Where they are not, only the pin separates the
  game from the other cards. Proton titles skip the namespace entirely, because Wine refuses to run as root.
- **No 2-card or 4-card assumptions.** `spirit roles` placed 9 of 10 cards on a made-up 10-card machine;
  the tenth had nowhere to plug a monitor, and the tool said so.
- **Intel is untested here.** The code selects Intel's VAAPI driver from the card's own kernel driver,
  but this project has only ever run on AMD hardware. Treat Intel support as written-and-unverified.

</details>

Copyright 2026 Sophia Marie DeClue. MIT.
