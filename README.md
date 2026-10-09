# Spiritual Successor

**Put every GPU in your PC to work.**

Most people with more than one graphics chip use one and leave the rest sitting idle. This tool puts
them all to work — two cards, four cards, ten, or just your processor's built-in graphics alongside a
real card.

The old CrossFire and SLI tried to split a single frame across two cards. That never worked well and
it's dead. We do the opposite: your game runs on **one** card — the one your monitor is plugged into,
which is almost always your best card — and every *other* card picks up a job of its own while you
play.

So a gaming card plus an old mining card, a datacentre card, or even your processor's built-in
graphics: all of them earn their slot.

No root, no install. Just Python (3.8 or newer).

## What it does

One command starts a game the right way:

```
spirit play -- ./yourgame
```

It does three things, in order:

1. **Finds the card your monitor is plugged into.** That card runs the game.
2. **Launches the game so it can only use that card.** The others are hidden from it.
3. **Starts a real video encode on every other card**, so they're working while you play.

Quit the game and the other cards stop.

## The thing everyone gets wrong

Leave a game alone and it will pick a graphics card *for* you. And it can pick the wrong one.

We watched it happen. A game with no pinning picked a slow card on an external dock instead of the
fast one. It ran at a crawl, and it got that slow card hot. The fast card sat there idle the whole
time, doing nothing.

That's the trap this tool closes. It makes your game use the card it should — the one your monitor is
plugged into.

## The commands

```
spirit doctor                  check the tool really works on your machine (run this first)
spirit probe                   show your cards, and how fast each can actually talk
spirit roles                   give every card a job — or say, plainly, why it can't have one
spirit play -- ./yourgame      run a game, with your other cards working underneath
spirit plan --workload ...     tell you honestly what a job can and can't do
spirit fg --res 1440p60        answer: can my spare card add frames to my game?
spirit run --role encode --card card2 -- ffmpeg ...    hand one job to one card
```

`spirit doctor` doesn't just describe — it **tests**. It launches a real program on each card and
checks the tool can see it, read it, and pin work to it. If something's wrong, it says so.

## What's true, and what isn't

Everything here was measured on our own machine — a fast new card alongside some older ones. We're
only telling you what we can stand behind.

- **Your game doesn't suffer.** Handing work to the other cards cost the main card little to nothing:
  about 1–2% in one game, nothing measurable in another, and in a full open-world session the player
  reported no stutter at all.
- **The win is the work the other cards do** — not extra frames. We are *not* promising your game runs
  faster.
- **We don't add frames yet.** Frame generation — making extra frames to raise your FPS — needs a fast
  link between the two cards. On our machine only the built-in graphics was fast enough, and we
  haven't finished that part. It's next.
- **One correction we're glad we caught:** we once thought a busy second card made the main card
  *faster*. It looked real. It didn't hold up on a real game, so we've marked it as a mistake and we
  don't quote it. (We got fooled a second time by something even simpler: the first run of any game
  is slow while it builds its shader cache, and that can look exactly like a gain. Warm it up before
  you compare.)

## Who this is for

Anyone with more than one graphics chip and a budget.

- **You have two or more GPUs** — a newer card plus an older one, or a couple of second-hand cards.
- **You have built-in graphics and a card as well** (or the other way around). Your processor's
  integrated graphics is a card here, not an afterthought.
- **You have a pile of old cards.** This is not a two-card trick. It doesn't care how many you have or
  who made them. We tested the job-assignment side against a made-up **10-card** machine and it placed
  9 of them — the tenth had nowhere to plug a monitor, and it says so.

If you've ever wanted a cheap second or third card to *do something* instead of sitting there, this is
for you.

## What this could do for you

It's early days, so we're careful about promises. Here's the honest shape of it:

- **Put more of your machine to work.** Your game runs on one card; the others take on real jobs at the
  same time. That means a gaming card plus old cards can do more at once — record or stream while you
  play, run a job in the background — without the game falling over.
- **The potential is more frames and better ray tracing on budget builds.** The whole idea is spreading
  work across several weaker cards. Several cheap cards together can, in principle, take on work that
  one of them couldn't alone. That's the direction we're building toward.
- **It spreads the load for games that want a lot of VRAM.** A game that needs more video memory than
  your main card has is a real wall. Handing the other work off to the other cards frees your main card
  up to hold the game.

## What this is NOT

**It is not a miracle.** It will not turn an old card into a new one. It will not let a GTX 1080 Ti run
Cyberpunk 2077 at 4K. It will not make a slow card fast, and it will not raise your frame rate today —
that part isn't built yet.

What it does is honest: it puts the cards you already own to work, and tells you the truth about what
each one can and can't do. If that's what you want, you're in the right place.

## Found a bug, or have an idea?

Email me: **sophia.wanders@proton.me**

Broken cards, things that didn't work, technical suggestions, or just a thought about how this should
work better — all welcome. **The more detail the better.** Tell me what cards you have, what game or
job you were running, what you expected, and what actually happened (error text helps a lot). A wall of
detail beats a one-line report every time.

---

<details>
<summary>For the technical reader — what's underneath</summary>

- **Links, not specs.** `lspci` and friends report the link between a GPU and the chip it sits behind,
  not the link to the CPU. A card on a dock or a chipset slot can read `x16` while its real host link
  is `x1`. `spirit probe` reports the **minimum negotiated width along the sysfs chain**, which is the
  truth. On the dev box: RX 9070 XT `x16 Gen5` (54.9 GB/s), Instinct MI25 `x4 Gen3` (3.19),
  Vega FE `x1 Gen3` (0.87).
- **Never split the frame.** A 1440p frame is 14.7 MB; at 60 fps an `x1` card needs 17 ms to move it,
  which is more than the whole frame budget. Per-pixel, per-frame work across these links is dead by
  arithmetic. Cross the wire once, or at frame rate — never per pixel.
- **The pin is load-bearing.** `MESA_VK_DEVICE_SELECT=<vid:did>!` — the trailing `!` is the exclusive
  flag. Without it the loader only *reorders* and a child still sees every GPU; with it, a child sees
  exactly one. Measured: an unpinned UE5 game rendered on the docked MI25 while the main card sat at
  0% busy.
- **Two walls.** The exclusive pin, plus a mount namespace that hides the other cards' `/dev/dri`
  nodes from the game. `spirit doctor` proves both, per card, on your machine.
- **A 10-card fleet** is placed 9/10 by `spirit roles` (the tenth is an iGPU with no panel; it says
  why). The engine makes no 2-card or 4-card assumptions.

Raw measurement logs sit beside this file.

</details>

Copyright 2026 Sophia Marie DeClue. MIT.
