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

## What the roles mean

`spirit roles` reads the machine and assigns, per card:

| role | what it does | needs |
|---|---|---|
| **render** | renders the game | the strongest link+VRAM card |
| **display** | owns the connected panel | a connected output |
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

## Roadmap

- **MVP-0 (done):** `probe` + `roles` + `plan` + `run`. Honest link truth, role assignment across N
  cards, feasibility arithmetic, and a device router.
- **MVP-1:** a *measured* cross-device DMA-BUF copy path (the negotiated link is not the real path —
  the copy usually bounces through system memory), so `plan` sizes against the real wire.
- **MVP-2:** a small daemon holding role leases, so interactive roles pre-empt batch jobs.
- **MVP-3:** the frame-gen/upscale present path, contributed *into* `lsfg-vk` / `gamescope` rather
  than as a new layer.

Copyright 2026 Sophia Marie DeClue. MIT.
