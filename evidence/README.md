# evidence

Raw telemetry from the measurement runs behind the numbers in `TEST-LOG.md`. These are the instrument
outputs, not write-ups — one JSON object per sample line, taken by the sampling scripts.

| file | what it is |
|---|---|
| `game_session.jsonl` | a full play session on the dev box — every card sampled per second |
| `multigpu_4card.jsonl` | the four-cards-four-jobs run: four real video files, one per card, all at once |
| `offload_encode.jsonl` | spare cards encoding while the gaming card stayed idle |
| `superposition_8k*.jsonl`, `super_*.jsonl` | Superposition 8K Optimized, capped vs uncapped; the power/score pair |
| `heaven_4k*.jsonl` | Heaven 4K, on the gaming card and with offload |
| `vkmark_*.jsonl` | VKMark baselines |
| `link_vs_clocks.jsonl` | link width against clock behaviour — the reason a docked card is link-bound, not age-bound |
| `order_test.jsonl` | proving the three orderings (Vulkan / DRM / `rocm-smi`) disagree, so cards are addressed by name |
| `idle.jsonl` | an idle machine, as the control |
| `explain_68.jsonl` | per-run explanations from a sweep |

These exist so that a claim can be checked against the instrument that produced it rather than against
the prose. The computed results are in `../TEST-LOG.md`. Nothing here is required to run the tool.
