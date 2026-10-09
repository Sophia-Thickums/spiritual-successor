# Spiritual Successor

**Put every GPU in the machine to work.**

CrossFire and SLI split one frame across cards and died for it. The Spiritual Successor does the
opposite: it **splits the work, not the frame**, and gives every card the role its own link and
silicon are actually good for — so a cheap second, third, or tenth GPU earns its slot instead of
sitting idle.

Scale is the point, not speed. The goal is that **every card in the box is doing something useful**
by the time the game is running. It scales to any number of GPUs, any vendor mix.

No root. No dependencies (stdlib Python 3.8+; `lspci` used only if present).

```
spirit probe                     # the TRUE host link, VRAM, outputs, render node of every GPU
spirit roles                     # assign a job to every card — all of them, or say why not
spirit plan --workload 1440p60-framegen
spirit run --role encode --card card2 -- ffmpeg -i in.mkv -c:v hevc_vaapi out.mkv
```

## The two truths it is built on (both measured — see the project folder)

**1. Every normal surface lies about a GPU's host link.** `lspci`, sysfs `current_link_width`, and
`amdgpu pp_dpm_pcie` all report the link between the GPU and **the bridge chip it sits behind** —
not the link to the CPU. A card on a dock or a chipset slot reads `x16` while the real host link is
`x1`. The truth is the **minimum negotiated width along the sysfs chain** from the root complex down.

    RX 9070 XT      x16 Gen5  54.9 GB/s   (measured)
    Instinct MI25   x4  Gen3   3.19 GB/s  <- lspci said x16
    Vega Frontier   x1  Gen3   0.87 GB/s  <- lspci said x16

**2. Per-frame multi-GPU rendering is dead on links like these.** A 1440p frame is 14.7 MB; at 60 fps
an x1 card needs 17 ms to move it — more than the whole frame budget. Splitting one frame across
cards needs ~25 GB/s+ between *same-class, same-driver* GPUs.

> **So: never split the frame. Split the process — and offload the pipeline tail.**
> Anything that crosses the wire at *frame rate* or *once* is fair game. Anything needed *this frame*
> is not, at any pipeline depth.

## ★ THE MONITOR LAW

**The GPU feeding the monitor is the main GPU — always, by rule.** The user plugs their display into
the card they mean to game on, and that is almost always the strongest one. Rendering anywhere else
would mean copying every finished frame across to the presenting card — a per-frame **pixel** copy,
which is precisely the one thing measured dead above.

So `spirit roles` makes the monitor-feeding card the renderer, unconditionally. If a *stronger* card
exists that feeds no display, it does **not** silently steal the main role — it takes a real offload
role (encode, compute) and the tool prints a note telling you to move the cable if you want it as main.
An unattended heuristic never overrules the user's own cabling.

## What the roles mean

`spirit roles` reads the machine and assigns, per card:

| role | what it does | needs |
|---|---|---|
| **render** | renders the game | **the card feeding your monitor — always** |
| **display** | owns the connected panel | same card as render, on purpose |
| **framegen / upscale** | interpolates and upscales on the display GPU | a *second* card that owns a panel |
| **encode** | VCN/VCE capture & streaming | any amdgpu (AMD has **no** concurrent-session limit) |
| **compute** | agents, LLM inference, Blender tiles | the largest VRAM off the render card |
| **analysis** | video analysis, proxies, preview bakes | any spare card (deadline-free) |
| **desktop** | drives the compositor so the gaming card keeps its VRAM | an integrated GPU with a panel |
| **compute-pool** | joins the system lane: renders, transcodes, agent jobs | any card with usable VRAM |
| **idle** | *stated with a reason* — never silently ignored | — |

Every card gets a job, or an explicit reason it cannot usefully take one. Nothing is faked.

## Scale

Tested against a synthetic **10-GPU heterogeneous fleet**: `spirit roles` places **9 of 10** cards
(the tenth is an integrated GPU with no panel attached, and it says exactly why). The engine is
N-generic — no 2-card or 4-card assumptions anywhere.

## Measured — what is proven, and what is dead

Every number below is reproducible; raw monitor JSONL sits beside this file.

**Proven on the dev box (RX 9070 XT + Instinct MI25 + Vega FE + iGPU):**
- **The link wall is real.** Shrinking the frame 76× multiplied the Vega cards' throughput ~45×
  (MI25 5,039→113 fps; FE 1,350→29) while the XT moved only 1.57×. Bytes-per-frame governs them.
  MI25/FE ≈ 3.7×, matching their x4/x1 link ratio of 3.67×. Clocks ruled out (identical 945 MHz memory).
- **Routing isolates.** Routed to each card, the child process sees exactly **one** GPU — the right one.
- **The first real offload works.** A 1080p60 H.264 VAAPI encode ran on the **MI25 at ~197 fps** while
  the XT rendered at 100% busy (54 MiB allocated on the MI25 vs 7 idle).
- **★ The feared penalty reversed.** With a GPU-bound load on the XT, running the offload on a second
  card left the main card **~6.25% faster** (32.0→34.0 fps) — proven order-controlled (A-B-B-A: 32/34/34/32).
  **Cause, identified:** with the second card busy the XT pins to **~3415 MHz / ~220 W**; idle it wanders
  3.10–3.27 GHz. Co-running a GPU holds the main GPU in its top clock/power state.
  *Scope: one synthetic GPU-bound load + one offload. Not yet generalised to games.*

**Dead ends — do not re-derive these:**
- **Unigine Superposition** — its 2019 engine mis-detects RDNA4 ("Unknown GPU / 256 MB") and forces
  1600×900 regardless of preset. Unusable headless. Use a modern Vulkan benchmark.
- **vkmark** — valid for routing and per-card capability; **useless for measuring GPU contention**
  (35,000 fps means it is CPU/present-bound, not GPU-bound).
- **`vkmark -D <uuid>`** — unusable here: all RADV devices report the *same* UUID. `MESA_VK_DEVICE_SELECT=vid:did!`
  (what `spirit run` emits) is the only way to pin work to one GPU.
- **`gpu_busy_percent` on Vega** — unreliable: the same FE read 71% and 0% with no load change.

## Roadmap

- **MVP-0 (done):** `probe` + `roles` + `plan` + `run`. Honest link truth, role assignment across N
  cards, feasibility arithmetic, and a device router.
- **MVP-1 (started):** a *measured* cross-device copy path, and VRAM budgeting in `plan` (a GPU-bound
  load costs ~10× the VRAM of a light one: 8.2 GiB vs 0.8 GiB measured).
- **MVP-2:** a daemon holding role leases, so interactive roles pre-empt batch jobs.
- **MVP-3:** the frame-gen/upscale present path, contributed *into* `lsfg-vk` / `gamescope`.
- **Packaging (last, per the owner):** GUI installer + easy uninstall, shipped as an AUR package and a Flatpak.

Copyright 2026 Sophia Marie DeClue. MIT.
