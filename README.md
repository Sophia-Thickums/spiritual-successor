# Spiritual Successor

**Your game keeps the card your monitor is plugged into. Your other graphics chips get a job — when you give them one.**

Spiritual Successor is a tool for a Linux machine with more than one graphics chip — two cards, four cards, or a card plus the graphics built into your processor. You play your game on the chip driving your display. While you play, your *other* chips can encode video on their own hardware encoders. Each card does its own job on its own, and the cards are not pooled or combined into one faster card — a `--split` job is divided across them and joined back into one file at the end.

```
spirit play --work video.mp4 --split -- ./yourgame
```

That cuts the video into pieces, encodes one piece on each card it will use, joins the pieces back into **one file**, and then checks on disk that the pieces add up to the whole before it claims anything worked.

**Give no `--work` and nothing is run on the other cards at all.** That is the default, and the tool says so both before it launches and again when your game exits.

How much does a job cost the game? In our own runs, on our own machine: a few percent at most in one title, and no measurable difference in another. Each side of that comparison was only two runs, so the honest reading is **small, or nothing measurable** — not a percentage, and not proof that it is exactly zero. Measure it on your own machine; that is the only number that matters.

---

## What you need

Read this before you install. It decides whether the tool can do anything for you.

**The machine**

- **Linux, and only Linux.** There is no Windows or macOS build, and none is planned. The tool drives Mesa, VAAPI, `unshare` and `gamescope`, which is why.
- **More than one graphics chip.** One chip on its own has nothing to offload to. A second card, an old card in a drawer, or the graphics built into your CPU all count.
- **An AMD or Intel GPU to take a job.** A job runs through a card's own hardware video encoder, and the tool drives what VAAPI exposes — **AMD** (the `radeonsi` VA driver, kernel driver `amdgpu`) or **Intel** (the `iHD` VA driver, kernel drivers `i915` and `xe`). Intel's `iHD` is Intel's own driver and is packaged separately; it is not part of Mesa.
- **Intel is written but has never been run.** This project has only ever been tested on AMD hardware. The Intel path is in the code and picks the right driver, but nobody has watched it work. Treat Intel as written-and-unverified.
- **Older AMD cards bind to a different kernel driver and are skipped.** The tool recognises `amdgpu`. GCN 1 and GCN 2 cards — roughly HD 7000, the R7/R9 200 series, and the later 300-series cards (R9 390, R7 360) that used the same silicon, plus the Kaveri/Kabini APUs — usually bind to the older `radeon` driver instead, and pre-GCN cards cannot take a job at all. You can force these onto `amdgpu` with a kernel parameter (newer kernels are moving that way); the tool will then report `can_encode: true`, though a very old encoder may still not handle the codec. `spirit probe` shows a skipped card as `can_encode: false` — that is correct, not a bug.
- **Intel's `iHD` covers Broadwell (2014) and newer.** Older Intel graphics need a driver (`i965`) that this tool does not map, so "the graphics built into your CPU" means a reasonably recent one.
- **NVIDIA is unverified here too.** An NVIDIA card can be the card your game runs on — but it cannot take a job (there is no encoder path for it here), and the game-pinning path has never been tested on one either. The tool names it as skipped rather than leaving it silently idle.

**The software it needs**

- **Python 3.8 or newer.** The installer stops if it is missing.
- **`ffmpeg`, built with VAAPI**, for the jobs. Nothing transcode-related works without it.
- **The VA driver for your card.** This is the piece people miss, because it is not part of the tool and not always part of Mesa. On AMD it usually ships *inside* Mesa — on Arch-family systems `radeonsi_drv_video.so` is in the `mesa` package with nothing separate to install; Debian and Ubuntu package it as `mesa-va-drivers`. On Intel it is separate: `intel-media-driver` on Arch, `intel-media-va-driver` on Debian and Ubuntu. **On Fedora, stock Mesa disables H.264 and H.265 *encoding***, and the job here is `h264_vaapi`, so jobs will fail until you install an encode-enabled build (RPM Fusion's `mesa-va-drivers-freeworld`). The Arch layout above was read off this project's own machine; the other package names come from their packaging documentation, not from a run here.
- **`cargo`, the GTK4 development headers, and `libadwaita`** — only if you want the desktop app built. The installer checks that all three are *present* and names what it needs if one is not; it cannot check that your versions are new enough for the app to compile. Everything else works without the app.
- **`pkg-config`** (or `pkgconf`), which the same check uses and which several distros omit from a minimal install.
- **Optional, used if present:** `gamescope` for upscaling, `unshare` for the second isolation wall, `vulkaninfo` if you want `doctor` to test device routing.

**On root:** the tool itself needs no root to install or run. Installing the packages above does need root, and the one optional piece that wants root permanently is the frame-generation witness, which you do not have to install.

**Where this has been run:** one Arch-family machine (CachyOS), AMD cards only. The Debian/Ubuntu package names below are written from the packages' own documentation, not from a run.

---

## What this is not

The things people get wrong about it, said before anything else:

- **It does not make your game faster.** Your other cards do not add a single frame to your game. The win is the work they do while you play.
- **It does not combine your cards into one bigger card.** That was SLI and CrossFire — making several cards behave like one faster card. This is not that. Nothing is shared between your cards. (A `--split` job is divided across cards and then joined back into one file at the end — that is re-assembly, not a faster card.)
- **It is not PRIME or `DRI_PRIME`.** Those usually do the opposite job: they move your game's *rendering* to the big card so a smaller card can run the display. Here your game stays on the card driving your display, and the other cards go and do something else entirely. (The tool sets `DRI_PRIME` as part of pinning a card, which is why the name appears below.)
- **It is not Lossless Scaling.** That scales a frame up and invents frames between frames. This tool does neither on its own — it moves work *off* your game's card. It can *drive* Lossless Scaling if you already own it (see **More frames**), but it does not include it, copy it, or reimplement it.
- **It does not assume two cards, or four.** A ten-card machine is handled the same way a two-card one is.
- **It does not record your gameplay.** The job it does itself is a *re-encode of a video file you already have*. If you want to record or stream, use `spirit run --role encode` and bring your own ffmpeg command.

---

## What it does

```
spirit play -- ./yourgame                        # your game, pinned to the right card
spirit play --work lecture.mp4 -- ./yourgame     # plus a real job on your other cards
```

1. It finds the card your monitor is on (the active output; failing that, a connected one; failing that, the fastest).
2. It launches your game so the game is pinned to that one card.
3. It runs a job on your other cards — a real transcode on each card's own encoder, written to real files.

**What the job actually produces.** Each card writes an H.264 (`h264_vaapi`) `.mp4` at about 8 Mbit/s, named after the input file and the card that made it. Output goes **beside the input file** unless you pass `--work-outdir DIR`. A re-run overwrites its own output. `--work` takes **one file** per run. Without `--split`, *every* card transcodes the whole file, so you get one full copy per card — `--split` is what divides one job across the cards and joins the pieces back.

**A card the tool cannot drive is named, not quietly skipped:**

```
  spare: SKIPPED renderD134 (…) — no VAAPI encoder this tool knows of (kernel driver: nvidia)
```

**About "which card can encode":** the tool decides that from the card's **kernel driver name** (`amdgpu`, `i915`, `xe`) — it maps a known driver to a known VA driver and skips anything it does not recognise. It does not interrogate the card's encoder capabilities. That is honest scope, not a claim: it is one decision made in one place rather than three, but it is still a decision from the driver name.

### It uses only the cards on a wide link, by default

Anything narrower than eight lanes — a card in a narrow slot, or on a riser or dock that is physically narrow — is left out, and named, unless you ask for it:

```
  job: NOT using renderD129 (Vega 10) — x4 link at the host — too narrow to use unasked
       Add --spare renderD129 to include it anyway.
```

A four-lane card is exactly what a docked or riser-mounted card looks like. On many consumer motherboards, the second full-length slot is electrically x4 through the chipset even though it looks full-size — so check with `spirit probe` before assuming a card will be used. Two things the rule is not: it counts **lanes, not the link's generation**, and it reads the width the chain will *allow* rather than the one currently negotiated — so a card on a cable that has trained down from a wider link can still read wide here and pass.

### Seeing that it actually happened

When your game exits, the tool prints what each card actually did. Here is the shape of it, on an example machine with a 12-second clip and two usable spare cards:

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
  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1 second).
  ── ONE OUTPUT FILE ───────────────────────────────────────────────────
  clip.joined.mp4  (10.8 MB, 12.0s)
  The whole input as one file, built from the 2 slices above.
  ── THE TAIL — what the other cards did ───────────────────────────────
  renderD131   GPU C                   REAL JOB  …/clip.renderD131_part1.mp4
  renderD129   GPU A                   REAL JOB  …/clip.renderD129_part2.mp4
```

Every job output is checked on disk afterwards — it exists, it was written by *this* run, and under `--split` its length is added up against the input to within **one second, or one percent of the input, whichever is larger**. On a clip shorter than 100 seconds the rule that applies is the one **second**, not the percentage. A file that did not get written says `MISSING` and the split says `INCOMPLETE`. **Nothing here takes a program's exit code for an answer.**

Under `--split` the pieces are then joined back into one file (`clip.joined.mp4`), whose own length is checked against the input before it is announced; the pieces stay on disk beside it. **Without `--split` there is no length check of that kind** — each card transcodes the whole input, and a shortened file still reports as written. The `--split` check is the one that proves the pieces reconstruct.

(Card names are whatever `lspci` calls them, shortened — shown here as placeholders. Run `spirit probe` on your own machine to see yours.)

---

## Install

```
curl -fsSL https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main/install.sh | sh
spirit doctor
spirit enable     # optional: pin every launch from now on, Steam included (nothing at boot)
```

The installer puts the **Python reference build** of the command into `~/.local/share/spirit`, a launcher into `~/.local/bin`, an icon and a menu entry. The C++ and Rust builds exist so that the three can be checked against each other; the installer does not install them.

If `~/.local/bin` is not on your `PATH`, the installer tells you the line to add.

**The installer tracks the `main` branch, not a release tag** — so the one-liner always installs the newest state of the project, which may be ahead of the release note you are reading.

**One thing to check if you installed earlier from `main`:** an earlier installer wrote
`~/.local/share/icons/hicolor/index.theme`, which is the desktop's fallback index for *every*
application and should not be yours. This installer no longer writes it; if that file exists and an
install of yours created it, delete it with `rm -f ~/.local/share/icons/hicolor/index.theme`.

The desktop app is the one part that has to be **built on your machine** — it needs `cargo`, `pkg-config`, the GTK4 development headers and `libadwaita`, and it takes a few minutes and a few hundred MB of temporary space. The installer checks that all of them are present before it starts, and names what it needs if one is missing; it does not check versions, so a distro shipping an old `cargo` or `libadwaita` can still fail at the build. Everything else works without the app.

```
Arch / CachyOS:   sudo pacman -S rust pkgconf gtk4 libadwaita
Debian / Ubuntu:  sudo apt install cargo pkg-config libgtk-4-dev libadwaita-1-dev
```

`spirit doctor` does not describe your machine — it **tests** it. It launches a real program on every card and checks that the card can be seen, read and pinned. Give it a minute.

**To update:** run the installer again. **To uninstall,** disarm the pin FIRST, then remove the files — in this order, because `spirit disable` is one of the things you are about to delete:

```
spirit disable      # first: removes the session pin and the persisted file
rm -rf ~/.local/share/spirit
rm -f  ~/.local/bin/spirit ~/.local/bin/spirit-gui
rm -f  ~/.local/share/applications/com.sophia.SpiritualSuccessor.desktop
rm -f  ~/.local/share/applications/spirit-gui.desktop
rm -f  ~/.local/share/icons/hicolor/scalable/apps/spirit.svg
rm -f  ~/.local/share/icons/hicolor/{16x16,22x22,24x24,32x32,48x48,64x64,128x128,256x256}/apps/spirit.png
rm -f  ~/.local/share/icons/hicolor/index.theme   # ONLY if an install of yours wrote it — see the note in Install
```

The **frame-generation witness** is the one piece that lives outside your home — it is a Vulkan layer installed with root. Its manifest sits under a system Vulkan layer directory (`implicit_layer.d`) and names the library it loads, usually wherever `present-counter/` was built; both need root to remove. See `present-counter/` in the repository for what it installs.

The pin file itself is `~/.config/environment.d/90-spirit.conf` — `spirit disable` removes it, and it is not touched by the `rm` lines above.

---

## The desktop app

There is a real program you click — GTK4 + libadwaita, no browser anywhere:

```
spirit-gui                # from a terminal
                          # or: find "Spiritual Successor" in your applications menu
```

It shows your cards and what the tool says each one can do, a box to give your other cards a job, the `doctor` check, and the on/off switch for the whole thing.

There is also a small **local page**, kept for when you have no display or you are working over SSH:

```
spirit ui                 # opens the page in your browser
```

It shows the same information, and it listens on your own machine only.

---

## Commands

```
spirit doctor        test that it really works on your machine — run this first
spirit enable        pin every launch from now on (Steam included); nothing at boot
spirit enable --persist   the same, and keep it across logins
spirit disable       disarm it, and remove the persisted pin
spirit --version     print the version and exit
spirit probe         list your cards and how each one is actually wired (the GB/s column is an estimate)
spirit roles         describe what each card is and what it could take
spirit plan          say what a given workload needs, and where it would fit
spirit play -- CMD   run a game on the right card; add --work FILE for a job on the others
spirit run --role R --card N -- CMD   run CMD with one card pinned for it
spirit boost         what extra frames this machine can give you, right now
spirit fg            the frame-pass estimate: what a resolution and mode would cost here
spirit origin        print this build's authorship notice (--check exits 0 for the original, 1 for a modified copy)
spirit ui            a local page showing your cards and what is available here
```

---

## Playing everything, Steam included, without thinking about it

Run this once a session:

```
spirit enable
```

It arms your session: every app you start from then on inherits the pin, so a game Steam launches when you click Play is pinned with no launch options to edit and this tool nowhere in the loop. **It is deliberately session-only** — nothing is written for boot, so a reboot always comes back with nothing armed. `spirit enable --persist` writes it for future logins as well; `spirit disable` undoes either.

**What it sets, exactly:** three environment variables — `MESA_VK_DEVICE_SELECT` (the exclusive device pin), `DRI_PRIME` (the render-node pointer), and `PROTON_FSR4_UPGRADE=1` (so Proton games can use FSR 4 on RDNA 4 cards). On hardware without FSR 4 that variable has nothing to act on — this project has not tested that case, and what Proton does with it depends on the Proton build. On disk it lives in `~/.config/environment.d/90-spirit.conf`.

Three limits, stated because they are real:

- **A Steam that was already running when you armed it did not pick the pin up** — its environment was fixed when it started. Restart Steam once.
- **The pin is exclusive for Vulkan only.** A native OpenGL game that Steam launches directly is steered by `DRI_PRIME` — usually onto the right card, but without the exclusive pin or the namespace. Use `spirit play` when you want the full separation.
- **Arming hides your other cards from Vulkan apps in the session**, a compute job included. That is the point for a game, and the reason it is not on by default.

If you would rather pin a single game by hand, put the tool *inside* Steam's launch line instead — right-click the game → Properties → Launch Options, and paste the full path to the launcher the installer created:

```
"$HOME/.local/bin/spirit" play -- %command%
```

Note the `--` before `%command%` — the game's own command line goes after it, untouched. (`~/.local/bin` is often not on Steam's `PATH`, which is why this is the full path to the wrapper rather than the plain name. The raw `spirit.py` file is not executable; only the wrapper is.)

---

## Offloading a job — the switch, and what it actually sets

There are two ways to give your other cards work.

**1. The built-in one — a transcode while you play.** This is the job the tool does itself:

```
spirit play --work lecture.mp4 --split -- ./yourgame
```

**2. The general one — pin any command to a card.** `spirit roles` says what each card is for; this sets the environment and runs your command:

```
spirit run --role <role> --card <card> -- <your command>
```

| role | what it sets | for |
|---|---|---|
| `encode` | `SPIRIT_VAAPI_DEVICE` to that card's render node (plus the pin) | recording, streaming, any ffmpeg VAAPI job — off the card your game is on |
| `compute` | the Vulkan device pin plus `GGML_VK_VISIBLE_DEVICES` | a Vulkan/ggml program; see the caveat below |
| `analysis` | the same, aimed at the batch lane | video analysis, proxies, preview bakes — work with no deadline |

Stream or record on a card that is not the one rendering your game:

```
spirit run --role encode --card card3 -- sh -c 'ffmpeg -vaapi_device "$SPIRIT_VAAPI_DEVICE" ...'
```

(The `sh -c '…'` matters: your shell would expand `$SPIRIT_VAAPI_DEVICE` *before* `spirit` runs, when it is still unset, and hand ffmpeg an empty device. Quoting it inside `sh -c` defers the expansion until the variable is set.)

**The honest limit: it sets environment variables and runs your command with them — that is all.** There is no mount namespace on this path, so the process can still see your other cards if it chooses to look; there is no `spirit stream` or `spirit transcribe`; and nothing here knows what your command is for. If you name the card your game is on, the tool will use it — it does not refuse. What is genuinely guaranteed is narrower and true: the variables that select a card name the card you asked for, and a program that honours them will use it.

**`--role compute` is not a general pin.** It sets `MESA_VK_DEVICE_SELECT` and `GGML_VK_VISIBLE_DEVICES`, which cover Vulkan and ggml's Vulkan backend. They do **not** cover ROCm or HIP: an AMD `ollama serve` normally uses ROCm, which reads `HIP_VISIBLE_DEVICES` / `ROCR_VISIBLE_DEVICES` and ignores the variables set here. The same goes for Blender's Cycles, which uses HIP or OptiX. **These examples have not been run on this project's machine** — if you need to pin a ROCm or CUDA program, set that program's own visible-devices variable.

---

## More frames — three things people mix up

**The upscaler matters more than the frame count.** On RDNA 4, AMD's FSR 4 is an ML upscaler built for that silicon. In a Proton game it is one flag — and it only upgrades a game that already ships FSR 3.1; a game with older FSR, or no upscaler at all, gets nothing from it:

```
PROTON_FSR4_UPGRADE=1 %command%
```

(`spirit enable` sets it for the whole session.) A silent fallback to FSR 3.1 looks identical, so turn on `PROTON_FSR4_INDICATOR=1` if you want to see which one is live. A game that already ships FSR 4 or DLSS needs none of this. (This project has not measured FSR 4's own cost — only its availability on this machine.)

**Upscaling through gamescope is FSR 1.0** — spatial, no motion vectors. It works on any card and any Vulkan game, but the picture falls apart well before FSR 4 does. Use it for titles with no in-game upscaler:

```
spirit play --scale 0.7 -- ./yourgame
```

On a 2560x1440 screen that renders 1792x1008 — 49% of the pixels. The trade is honest: you pay in resolution.

**Frame generation** invents frames between the real ones. It needs `lsfg-vk`, which needs you to own Lossless Scaling. It multiplies whatever the upscaler bought you:

```
spirit play --framegen 2 -- ./yourgame
```

Measured on this project's machine at 2560x1440: **2x costs 0.71 ms, 3x 1.18, 4x 1.66** — the tax is small, so size the multiplier so base × N lands near your panel's refresh: on a **165 Hz panel** that is 2x from ~80, 3x from ~55, 4x from ~41. On a 60 Hz or 144 Hz panel the numbers differ. Run `spirit boost` first: it says what is and is not available here. Frame generation can look wrong on fast movement — that is the technology, not this tool.

**Three things to know before you try it, so it does not look broken when it is not.**

1. **You install it, not this tool.** Frame generation is a separate project (`lsfg-vk`) and it needs your own paid copy of Lossless Scaling. This tool does not include it, copy it, or reimplement it. If you do not own it, nothing here will make frame generation appear.
2. **It does nothing until something tells it which settings to use.** The layer loads into every Vulkan game on your machine and then sits there silently. `--framegen` is what wakes it: it makes sure a profile exists for the multiplier you asked for, and points the layer at it. If you have never opened lsfg-vk's own settings program, this tool will say so and then launch your game **without** frame generation — it will not write a settings file for a program it does not own.
3. **The numbers above are this machine's, not yours.** They come from one card on one 2560x1440 screen. Your card and your screen decide your multiplier — which is why the tool asks you for one instead of choosing for you.

### The frame-count witness

If you use frame generation, the tool prints the one number frame generation actually moves — and it measures that itself, from *below* the frame-gen layer, with a small Vulkan layer it ships (`present-counter/`). No game counter and no overlay can see those frames. On the test machine, 300 frames of work produced **300** presented images with frame generation off and **1200** with it on at 4x.

Your game's own counter is a different thing and it cannot see them: it counts the frames the game rendered, and with frame generation on the game may be doing less work, because it is now paying for the interpolation. On one title here that figure fell about 9%; on another it did not move at all.

What the witness does **not** tell you is whether the invented frames *look right*. No counter can. That one you look at.

---

## Known limits

Honest list. None of these is hidden in the code — each one is a real edge.

- **Arming only reaches apps started after it.** A Steam that was already running keeps the environment it started with — restart it once.
- **`spirit enable` delivers the device pin, not the second wall.** The mount namespace that hides the other cards' device nodes applies to games launched through `spirit play`, not to ones Steam starts by itself.
- **The pin is exclusive for Vulkan only.** `MESA_VK_DEVICE_SELECT` makes a Vulkan program see exactly one card. OpenGL is instead steered by `DRI_PRIME`, which usually puts a native OpenGL game on the right card but does not hide the others from it.
- **The pin is written once and never re-checked.** Move your monitor to another card, or add a card that renumbers PCI addresses, and the pin still names the old one. Re-run `spirit enable` whenever what drives your display changes.
- **A `~/.config` that is a symlink** (a stow/dotfiles setup) takes the pin file into that repository, where it names a card that means nothing on another machine. `spirit disable` before you commit.
- **`XDG_CONFIG_HOME` is not honoured** — the pin is always written under `~/.config`.
- **A session-wide pin hides your other cards from Vulkan apps in the session**, a compute job included. That is the point for a game; on a machine that also computes on those cards, leave it off and use `spirit play` per launch.
- **A split is joined by stream copy, so slices from *different* encoders are joined, not proven seam-clean.** Each slice is encoded by the card that took it. Two AMD cards carrying the same encoder revision (VCN, or VCE on the older parts) will normally produce the same kind of stream — but different revisions can differ in profile and stream parameters, and an old card and a new one in one machine carry different revisions. So a mixed-generation AMD split — and any AMD+Intel split — is the same "different encoders" case. The joined file's total length is checked against the input; the seam between two different encoders' output is not, because nothing here can verify it. If you split across makers or generations and the result looks wrong mid-file, re-encode the joined file once. **A mixed AMD+Intel split has never been run.**
- **Split boundaries land on keyframes.** The tool seeks by keyframe, so a slice may begin slightly before or after its nominal second. The check adds the pieces up and compares them to the input, which catches a missing or truncated slice — it cannot see a fraction of a second of overlap or gap at a seam.
- **Which card your game runs on is the card driving your display, not the strongest one.** On a desktop that is usually what you want. But read this before you trust it: if you plug your monitor into the motherboard, the game is pinned to the **integrated graphics**; if you plug it into an older card, the game is pinned to that older card. On a laptop the built-in panel is very often wired to the integrated graphics, so the rule picks the *weaker* chip there. Run `spirit roles` first and see what it picks.
- **An integrated GPU counts as a card, but its "link" reading is the CPU root port, not a card link**, and many motherboards disable the integrated GPU in firmware when a discrete card is present. `spirit probe` says when it is looking at an integrated chip. Treat its GB/s figure as meaningless.

---

## Who this is for

Anyone on Linux with **more than one graphics chip, and at least one AMD or Intel card to take the job**. The card you play on can be any vendor, NVIDIA included — but a job has to land on an AMD or Intel card, so a machine with, say, an NVIDIA display card and an NVIDIA spare can do nothing here **unless your CPU's own graphics is enabled** (that is an AMD or Intel chip, and it can take the job).

If you have an old AMD card in a drawer and a slot to put it in, this can turn it back into something useful instead of a paperweight — provided it binds to the `amdgpu` driver (see **What you need**). Same for AMD or Intel graphics built into your CPU, which sits idle the whole time you play.

---

## Telling me it went wrong

Email **sophia.wanders@proton.me**. Tell me your cards, the game or the job, what you expected and what actually happened. A wall of detail beats a one-line report.

---

<details>
<summary>For the technical reader — what is underneath</summary>

- **Links, not specs.** `lspci` and friends report the link between a GPU and the chip it sits behind, not the link to the CPU. A card on a dock or a chipset slot can read `x16` while its real host link is `x1`. `spirit probe` reports the **minimum link width along the sysfs chain** — the capability the chain gives the card, not the rate it has currently negotiated; the negotiated one is shown separately as `[idle now: x…]` when a card is sitting idle. The `GB/s` column beside it is an *estimate* — theoretical per-lane rate times width times a fixed efficiency — not a measurement; treat it as a ranking, not a speed.
- **Never split the frame.** A 1440p frame is 14.7 MB; at 60 fps a Gen3 `x1` card needs about 17 ms to move one, which is more than the whole frame budget — and a Gen4 `x1` card about 8.5 ms to move it *out*, with the same again to bring it back, so a round trip is roughly a full budget too. Per-pixel, per-frame work across these links is dead by arithmetic. Cross the wire once per job, or at frame rate — never per pixel.
- **The pin is load-bearing.** `MESA_VK_DEVICE_SELECT=<vid:did>!` — the trailing `!` is the exclusive flag. Without it the loader only *reorders* and a child still sees every GPU; with it, a child sees exactly one. Measured: an unpinned UE5 game rendered on a card it should not have, while the card driving the display sat at 0%.
- **Two walls.** The exclusive pin, plus a mount namespace that hides the other cards' `/dev/dri` nodes from the game. `spirit doctor` reports both: it tests the pin per card, and it says plainly whether unprivileged namespaces are available on your system. Where they are not, only the pin separates the game from the other cards. Proton titles skip the namespace entirely, because Wine refuses to run as root.
- **No 2-card or 4-card assumptions.** `spirit roles` placed 9 of 10 cards on a simulated 10-card machine; the tenth had nowhere to plug a monitor, and the tool said so.
- **Three builds of the same tool.** The command exists in Python (the reference), C++ and Rust. Two differential test suites compare each of the others against the reference — on a fake fleet and on the live machine. A green suite proves the three builds agree **on the cases the suites contain**; it does not prove any of them is right — a fault all three share is invisible to a comparison between them — which is why every release also goes to an outside reader before it ships.
- **Intel is untested here.** The code selects Intel's VAAPI driver from the card's own kernel driver, but this project has only ever run on AMD hardware. Treat Intel support as written-and-unverified.

</details>

---

Copyright 2026 Sophia Marie DeClue. MIT.
