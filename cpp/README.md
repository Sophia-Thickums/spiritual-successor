# spirit — C++ port

A C++17, standard-library-only, drop-in behavioural match for the Python original
`../spirit.py` (the *Spiritual Successor*). Same subcommands, same flags, same
human-readable output shape, same JSON structure.

## Build

```sh
make                 # -> ./spirit   (g++ by default; CC/CXX overridable)
make CXX=clang++     # clang++ also supported
```

or with CMake:

```sh
cmake -S . -B build && cmake --build build
```

## Test

```sh
make test            # runs ./run_tests.sh — differential vs ../spirit.py
```

`run_tests.sh` runs both binaries with an identical environment and diffs their
stdout/stderr/exit codes, in two modes:

* **FAKE** — `SPIRIT_DEVICES` points at `../fleet-10.example.json` (a deterministic
  10-GPU fleet), passed to each **child** via `env` (never exported into the test
  shell): `probe`, `roles`, `plan` (all workloads), `fg`, `doctor`, `boost`, `run`,
  `play` (including `--scale` and `--framegen`).
* **REAL** — `SPIRIT_DEVICES` unset: the live box.

`unshare`, `ffmpeg`, `vulkaninfo` and `gamescope` are stubbed on `PATH` so `run` and
`play` can be verified — the environment `run` sets, and the exact argv `play` spawns
(the `gamescope` wrapper for `--scale` included) — without touching a real GPU or a
real display. In REAL mode the stubs are removed.

## Files

| file | purpose |
|---|---|
| `spirit.cpp` | the whole program (one translation unit) |
| `Makefile` | plain `make` build |
| `CMakeLists.txt` | CMake build |
| `run_tests.sh` | differential test harness vs the Python original |

## What was ported faithfully

* **The sysfs PCIe-link walk** (`chain_for` / `binding_link` / `link_gbps`): the
  MINIMUM negotiated width and speed independently, taken along the whole chain from
  the root complex — *not* the card's own `current_link_width` (which lies). Currents
  and maxima are minimised independently; the binding element is the first chain
  element holding the minimum current width.
* **`probe()`** — the card DB, built from `/sys/class/drm/card*`, `lspci -mm`, sysfs
  `uevent`/`mem_info_vram_*`, connector status, render nodes; sorted by `gbps_max`
  descending. Also honours `SPIRIT_DEVICES` (loads and re-emits an arbitrary DB),
  with its own small JSON parser/serialiser so the fake-DB path matches byte-for-byte.
* **`assign_roles()` / `score()`** — the same role ladder (render, display, framegen,
  upscale, encode, compute, analysis, desktop, compute-pool) with the same tie-breaking
  (Python `max`/`min` keep the first extreme) and the same reason strings.
* **`WORKLOADS` and `plan()`** — same arithmetic, verdicts and `--vram-mib` override.
* **`run()`** — `DRI_PRIME=pci-<bdf>` and `MESA_VK_DEVICE_SELECT=<vid>:<did>!` (the
  trailing `!` is the EXCLUSIVE flag), plus `GGML_VK_VISIBLE_DEVICES` and
  `SPIRIT_VAAPI_DEVICE` for the right roles; then executes the command with that env.
* **`play()`** — picks the monitor card, spawns `unshare -rm sh -c '<bind-mount wall>'`
  hiding the other cards' `/dev/dri` nodes, starts real h264 VAAPI encoders on the
  spares (the VAAPI driver is chosen per card from its own kernel driver; a card with no
  known encoder is named as SKIPPED), launches the game, then polls the encoders before
  terminating them — so the tail reports what each card actually did, not what we hoped.
* **`fg()`** — the frame-gen handoff arithmetic, including the original's `--res`
  parsing quirk.
* **`doctor()`** — probe-links / routing-isolation / telemetry / role-plan checks, then
  the two frame-boost checks **upscaling** (`_gamescope_state`) and **frame-generation**
  (`_lsfg_state`), with PASS / FAIL / UNKNOWN, never a silent pass (only FAIL is bad).
* **`boost()`** — the upscaling/frame-generation availability report: gamescope state
  (its `--help` is read from **both** stdout and stderr — gamescope prints it all to
  stderr), the FSR scale table for the monitor card's native mode, and the lsfg-vk state.
* **`_lsfg_state()`** — checked against the surfaces that own the answer, *not*
  `lsfg-vk-cli validate`: the layer manifest, the Steam `Lossless Scaling` install
  (`Lossless.dll`) and the `appmanifest_993090.acf` branch key (`betakey` / `"lsfg-vk"`),
  yielding STOOD-DOWN / ABSENT / NEEDS-GAME / NEEDS-BRANCH / PARTIAL / INSTALLED.
* **`play --scale F` / `play --framegen N`** — hand-parsed like `run`/`play`; `--scale`
  in `[0.3, 1.0]` and `--framegen` in `{2,3,4}` else exit 2 with the exact stderr message.
  `--scale` prepends the `gamescope … --fsr-sharpness 2 --` wrapper (REFUSED, exit 1, if
  gamescope is not READY or the native mode is unreadable); `--framegen` sets
  `LSFGVK_PROFILE` (and `ENABLE_GAMESCOPE_WSI=0` under `--scale`) or warns, never fails.

## Known limitations

* Python `%`-width padding counts *characters*; this port uses `printf` widths, which
  count *bytes*. They differ only when a padded field contains non-ASCII (not the case
  for any string field on the tested hardware).
* `argparse` abbreviation matching (`spirit probe --j`) is not implemented; exact long
  flags are. All help text and error messages for the tested flags match byte-for-byte.
* `doctor --json` exists as an internal function (as in the original) but, like the
  original, is not reachable from the CLI.

Copyright 2026 Sophia Marie DeClue. MIT.
