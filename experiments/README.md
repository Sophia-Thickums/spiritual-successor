# experiments

One-off scripts from the work that led to `spirit.py`. They are kept because they are the provenance of
several findings, and because a reader may want to see how a measurement was actually taken.

**They are not part of the tool, they are not installed, and nothing here is maintained.** The tool is
`../spirit.py` (and its C++ and Rust ports in `../cpp/` and `../rust/`).

| file | what it did |
|---|---|
| `multigpu.py` | early multi-card orchestration |
| `offload_demo.py`, `offload3.py` | the offload experiments, including the synthetic-load path that later turned out to be a load and not a job |
| `monitor.py` | per-second sampler for cards, clocks and temperatures |
| `contend.py` | contending two workloads for one card, to find where they interfere |
| `ab_toggle.py` | toggling a run on and off around a benchmark |
