# spirit (Rust) — Spiritual Successor, ported from `spirit.py`

A faithful Rust port of the stdlib-Python `spirit` CLI at
`../spirit.py`. Drop-in behavioural match: same subcommands, same flags, same
human-readable output text, the same JSON structure and the same exit codes.

**Rust standard library only — no crates** (mirrors the original's "no dependencies"
promise). The binary shells out to `lspci`, `vulkaninfo`, `gamescope`, `unshare`,
`ffmpeg` *only when they are present*, exactly as the Python does.

Current parity target: **spirit.py v0.2** (the `boost` command, `play --scale` /
`play --framegen`, and the two new `doctor` checks).

## Build

```
cargo build --release        # produces target/release/spirit
```

Built and tested with the system toolchain: `cargo 1.99.0` / `rustc 1.99.0`
(Arch `rust` package) — nothing needed installing.

## Test

```
./test.sh                    # fake-fleet parity harness vs ../spirit.py
```

`test.sh` diffs the Rust binary against `python3 ../spirit.py` on identical inputs and
reports PASS/FAIL per case. It reads the Python and the example fleets in the parent
directory but **writes nothing outside a temp dir**. It uses only fake commands
(`/bin/echo`, `/bin/sh -c`) so no game and no real encode is launched. It sets
`SPIRIT_DEVICES` **per child command** (never `export`ed), so a fake fleet can never
leak into the caller's shell.

Covers: probe / probe --json / roles / roles --json / plan (all 4 workloads + `--vram-mib`
+ unknown) / fg (both modes) / boost / doctor / run (env pins, exit-code propagation,
errors) / the `play --scale` & `play --framegen` argument surface / the whole `--help`
& argparse-error surface — on both the real machine and the two `*.example.json` fake fleets.

## What was ported faithfully

- **The sysfs PCIe-link walk** (`chain_for`, `binding_link`, `link_gbps`): the true host link
  is the **minimum negotiated width/speed along the chain from the root complex**, not the
  card's own `current_link_width` (lspci/sysfs report the link to the card's own bridge).
  Widths and speeds are minimised independently; the *maxima* drive sizing/sorting because
  `current_link_*` drops at idle and would reorder the table run-to-run.
- **`probe()`** card DB, `score()`, `assign_roles()` (the 8-step role ladder), the
  `WORKLOADS` table, `plan`, `run`, `play`, `fg`, `doctor` — line-for-line logic.
- **`run`** pins one card: `DRI_PRIME=pci-<bdf>`, `MESA_VK_DEVICE_SELECT=<vid:did>!` (the
  trailing `!` is the EXCLUSIVE flag), plus `GGML_VK_VISIBLE_DEVICES` for `compute` and
  `SPIRIT_VAAPI_DEVICE` for `encode`.
- **`play`** spawns a user/mount namespace (`unshare -rm sh -c …`) hiding every other
  card's `/dev/dri/<card>` and `/dev/dri/<renderDn>` node behind `/dev/null`, starts a real
  `ffmpeg` VAAPI H.264 encode on every spare render node, launches the game, then polls the
  encoders before terminating them. **`LIBVA_DRIVER_NAME` is chosen per card** from its own
  kernel driver — `amdgpu` → `radeonsi`, `i915`/`xe` → `iHD` — and a card with no encoder this
  tool knows of is reported as `SKIPPED` rather than launched and left to fail silently. The
  exact `unshare` argv and the exact ffmpeg argv were diffed against the Python and are
  byte-identical.
- **JSON output** matches `json.dumps(..., indent=2, ensure_ascii=True)` exactly, including
  key order, `\uXXXX` escaping (e.g. the em-dash in the role "why" strings), and the int/float
  distinction (a whole-number float prints as `54.0`, never `54`).

## v0.2 additions (ported from the updated original)

- **`boost`** — an upscaling / frame-generation availability report. Reads the live
  gamescope state (via `gamescope --help`, **both** streams — its whole help goes to
  *stderr*), the lsfg-vk state (layer manifest, Steam `Lossless Scaling` install, and the
  Steam appmanifest's `betakey` / `"lsfg-vk"` branch markers), and the monitor card's
  preferred mode from sysfs; prints the `--scale` render-resolution table and an honest
  availability line for each path.
- **`play --scale F`** (float, valid range `0.3`–`1.0` inclusive; out of range or non-numeric
  is exit **2** with the exact stderr message). On a valid value it **prepends** a
  `gamescope -w .. -h .. -W .. -H .. -f -F fsr --fsr-sharpness 2 --` argv, and **REFUSES**
  (exit 1, no launch) when gamescope is not `READY` or the native resolution can't be read.
- **`play --framegen N`** (int, must be 2/3/4; else exit **2**). When the lsfg-vk state is
  available it sets `LSFGVK_PROFILE` (and, under gamescope, `ENABLE_GAMESCOPE_WSI=0`); it
  **only warns, never fails**, when frame-gen is unavailable.
- **`doctor`** gains two checks before the overall verdict: `upscaling`
  (`PASS` iff gamescope `READY`, else `UNKNOWN`) and `frame-generation`
  (`PASS`/`FAIL`/`UNKNOWN` mapped from the lsfg state). The overall rule is unchanged:
  only `FAIL` is bad, `UNKNOWN` is not.
- The top-level description and argparse `--help` now list `boost`
  (`{probe,roles,plan,doctor,boost,fg}`; description `… fg · boost · run · play …`).

## Known deviations (small, stated honestly)

1. **Encoder teardown signal.** Python `Popen.terminate()` sends **SIGTERM**; the Rust
   `std::process::Child::kill()` sends **SIGKILL**. `ffmpeg` dies either way and the child is
   reaped, but the signal differs. (std has no SIGTERM without `libc`/`nix`.)
2. **Game killed by a signal.** Python `subprocess.call` returns a negative signal number in
   that case; the Rust child-status code handles the common path via `ExitStatus::code()`
   (and `signal()` on unix for `run`), but the `play` teardown path is not exercised by tests.
3. **Glob ordering.** Python `glob.glob` returns `os.scandir` order; the Rust helper sorts.
   Only observable if a single card exposed more than one `renderD*` node (never happens).
4. **`run --role` with no value** (flag is the last arg): Python raises `IndexError` (traceback,
   exit 1); Rust treats it as a missing flag and prints the usage (exit 1). Same exit code,
   different stderr.
5. **Concurrent spawn order of the encoders.** Both start one encode per spare card in the
   same order; because they run concurrently the *interleaving* of their log lines can differ.
   The *set* of spawned commands is identical.
6. **`gamescope --help` / child-process failure strings.** Where the original formats a caught
   exception (`%s`), the Rust prints its `io::Error` display, which differs in wording but not
   in the state/verdict. Not reachable on a box where the tool is actually installed.
7. `lspci`'s device-name string, `vulkaninfo`'s device count and gamescope's own log come from
   those tools, so they are whatever the host reports (identical for both implementations on
   the same host), which is exactly what the differential tests assert on this box.

## Layout

```
Cargo.toml
src/main.rs      the whole program (single file, std only)
test.sh          parity harness vs ../spirit.py
```
