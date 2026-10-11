#!/bin/sh
# parity test — diff the Rust `spirit` against the Python original on identical inputs.
# Usage:  ./test.sh            (uses the parent spirit.py + example fleets)
#         PY=/path/to/spirit.py ./test.sh
#
# It builds nothing and modifies nothing; it only reads the Python and runs both CLIs.
#
# SPIRIT_DEVICES is set PER CHILD COMMAND (never exported), so it cannot leak into the
# caller's shell and poison later readings with a fake fleet.
#
# THREE THINGS THIS SUITE USED NOT TO CHECK, AND NOW DOES (fix 2026-10-11):
#   * PATH stubs for unshare / ffmpeg / ffprobe / vulkaninfo / gamescope. Before this, a `play`
#     case ran the REAL `unshare -r` on the live box, and the Rust spawn argv — including the
#     LIBVA_DRIVER_NAME the offload hands each encoder — was never compared at all.
#   * offload cases: --work, --split, --work-outdir, --spare, and the three "no job" reasons.
#   * a real-box phase (SPIRIT_DEVICES unset, no stubs), plus the `--opt=value` argument form.
#
# The spawn argv is compared by diffing the stub logs; the ffmpeg log is SORTED first because
# the spare encoders start concurrently and their lines interleave in a different order each run.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PARENT=$(cd "$HERE/.." && pwd)
PY=${PY:-$PARENT/spirit.py}
RS=${RS:-$HERE/target/release/spirit}
FLEET10=$PARENT/fleet-10.example.json
FLEETWEAK=$PARENT/fleet-monitor-on-weak.example.json

[ -x "$RS" ] || { echo "build first: cargo build --release" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 required" >&2; exit 1; }
# ★ ASSERT THE FIXTURES EXIST (B2). If a fake-fleet fixture is missing, every case that reads it
# silently becomes "both sides refuse identically" and PASSES. Fail loudly instead.
[ -f "$FLEET10" ]   || { echo "test.sh: fixture missing: $FLEET10" >&2; exit 1; }
[ -f "$FLEETWEAK" ] || { echo "test.sh: fixture missing: $FLEETWEAK" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

STUB="$TMP/bin";          mkdir -p "$STUB"           # every stub
NOFFMPEG="$TMP/bin-noff"; mkdir -p "$NOFFMPEG"       # no ffmpeg  -> "ffmpeg is not installed"
NOFFPROBE="$TMP/bin-nofp";mkdir -p "$NOFFPROBE"      # no ffprobe -> split: REFUSED
NOGS="$TMP/bin-nogs";     mkdir -p "$NOGS"           # no gamescope -> overlay: REFUSED
HANG="$TMP/bin-hang";     mkdir -p "$HANG"           # gamescope --help never returns
NOTOOLS="$TMP/bin-notools"; mkdir -p "$NOTOOLS"      # no vulkaninfo/unshare/gamescope -> doctor UNKNOWNs
UNLOG="$TMP/un.log"; FFLOG="$TMP/ff.log"; GSLOG="$TMP/gs.log"
: > "$UNLOG"; : > "$FFLOG"; : > "$GSLOG"
REAL_PATH=$PATH
PATHVAL=$REAL_PATH       # what the children get; never exported into this shell

# ── fixtures (nothing is written outside $TMP) ────────────────────────────────────────────────
python3 - "$TMP" <<'PYEOF'
import json, os, sys
d = sys.argv[1]
def gpu(card, bdf, name, driver, vram, out, node, gbps=13.5, integ=False, conn=3):
    return {"card": card, "bdf": bdf, "name": name, "driver": driver,
            "vendor": "0x1002", "device": "0x0000", "integrated": integ,
            "render_node": node, "vram_total_mib": vram, "vram_used_mib": 0,
            "display_outputs": 1 if out else 0, "connectors_total": conn, "has_output": out,
            "link": {"binding_element": "0000:00:00.0", "max_width": 16, "max_gen": None,
                     "gbps_max": gbps, "current_width": 16, "current_gen": None,
                     "gbps_current": gbps, "idle_degraded": False,
                     "own_reports_width": 16, "capped_by_topology": False}}
def dump(name, gpus):
    with open(os.path.join(d, name), "w") as fh:
        json.dump({"gpufan": "probe/1.0.0", "gpus": gpus}, fh)
# ★ A card name whose 22nd BYTE falls INSIDE a multi-byte character: `™` is E2 84 A2 at bytes
# 21..23, so a byte-index `[..22]` slices mid-character. Python cuts at 22 CODE POINTS.
dump("unicode.json", [
    gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, True, "renderD128", 54.4),
    gpu("card1", "0000:02:00.0", "RX 6800", "amdgpu", 16384, False, "renderD129"),
    gpu("card2", "0000:03:00.0", "AMD Radeon RX 7900 XT\u2122 24GB", "nvidia", 24576, False, "renderD130"),
])
dump("nogpu.json", [])
dump("one.json", [gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, True, "renderD128", 54.4)])
dump("two.json", [
    gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, True, "renderD128", 54.4),
    gpu("card1", "0000:02:00.0", "RX 6800", "amdgpu", 16384, False, "renderD129"),
])
dump("noenc.json", [
    gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, True, "renderD128", 54.4),
    gpu("card1", "0000:02:00.0", "Tesla P40", "nvidia", 24576, False, "renderD129"),
])
# ★ The main card is card1 here, because card1 is the one on THIS box whose connected output
# publishes a preferred mode — so `--scale`/`--overlay` get past the native-resolution gate and
# the gamescope argv is actually built and spawned. On a box where no mode is readable both
# builds simply refuse, and the case still holds (they agree on the refusal).
dump("modes.json", [
    gpu("card1", "0000:02:00.0", "RX 6800", "amdgpu", 16384, True, "renderD129", 54.4),
    gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, False, "renderD128"),
])
# ★ DOCTOR FIXTURES (C): real card NAMES, so the live sysfs telemetry / connector reads apply, and
# explicit vendor:device so one fleet PASSES the individual-pin check and one DUPLICATES it.
def dgpu(card, bdf, name, out, gbps, vid="1002", did="0001", integ=False, mw=16, gen="Gen5"):
    return {"card": card, "bdf": bdf, "name": name, "driver": "amdgpu",
            "vendor": "0x" + vid, "device": "0x" + did, "integrated": integ,
            "render_node": "renderD" + card[4:], "vram_total_mib": 16384, "vram_used_mib": 0,
            "display_outputs": 1 if out else 0, "connectors_total": 3, "has_output": out,
            "link": {"binding_element": "0000:00:00.0", "max_width": mw, "max_gen": gen,
                     "gbps_max": gbps, "current_width": mw, "current_gen": gen,
                     "gbps_current": gbps, "idle_degraded": False,
                     "own_reports_width": mw, "capped_by_topology": False}}
dump("onecard.json", [dgpu("card1", "0000:03:00.0", "RX 7900 XTX", True, 54.4)])
dump("dup.json", [dgpu("card1", "0000:03:00.0", "RX 7900 XTX", True, 54.4, vid="1002", did="0000"),
                  dgpu("card2", "0000:11:00.0", "RX 6600", False, 0.85, vid="1002", did="0000",
                       mw=1, gen="Gen3")])
# ★ A fleet that can PASS every doctor check (exit 0): TWO distinct cards, so the routing-isolation
# check gets past its one-GPU UNKNOWN, and each vendor:device is unique so individual-pin PASSES.
# Used with the smart-vulkaninfo stub (below), which honours the pin like the real one.
dump("doctorok.json", [dgpu("card1", "0000:03:00.0", "RX 7900 XTX", True, 54.4, vid="1002", did="744c"),
                       dgpu("card2", "0000:11:00.0", "RX 6600", False, 13.5, vid="1002", did="73ff",
                            mw=16, gen="Gen4")])
# ★ SPLIT FIXTURES: non-marginal, encodable spares so a real SPLIT runs and the on-disk
# COMPLETE/INCOMPLETE arithmetic is exercised (2 parts sum back; 3 do not).
dump("split2.json", [
    gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, True, "renderD128", 54.4),
    gpu("card1", "0000:02:00.0", "RX 6800", "amdgpu", 16384, False, "renderD129"),
    gpu("card2", "0000:03:00.0", "RX 6600", "amdgpu", 8192, False, "renderD130"),
])
dump("split3.json", [
    gpu("card0", "0000:01:00.0", "RX 9070 XT", "amdgpu", 16304, True, "renderD128", 54.4),
    gpu("card1", "0000:02:00.0", "RX 6800", "amdgpu", 16384, False, "renderD129"),
    gpu("card2", "0000:03:00.0", "RX 6600", "amdgpu", 8192, False, "renderD130"),
    gpu("card3", "0000:04:00.0", "RX 5700", "amdgpu", 8192, False, "renderD131"),
])
PYEOF
: > "$TMP/job.mkv"          # a real (tiny, empty) input for --work
mkdir -p "$TMP/nonfile-dir" # ★ a DIRECTORY (B3): the --work case must hit the REFUSED branch, not the normal path

# ★ ISOLATE HOME FOR EVERY --framegen CASE (B4). `_ensure_lsfg_profile` APPENDS to
# ~/.config/lsfg-vk/conf.toml and copies it aside; a test run must never touch the user's real config.
# This fake HOME carries a Lossless Scaling install on the lsfg-vk branch and a conf that ALREADY
# names 'spirit 2x', so ensure_lsfg_profile takes the read-only "already present" path.
FAKEHOME="$TMP/fakehome"
mkdir -p "$FAKEHOME/.config/lsfg-vk" \
         "$FAKEHOME/.local/share/Steam/steamapps/common/Lossless Scaling"
: > "$FAKEHOME/.local/share/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
printf '"BetaKey" "lsfg-vk"\n' > "$FAKEHOME/.local/share/Steam/steamapps/appmanifest_993090.acf"
printf '[[profile]]\nname = "spirit 2x"\nmultiplier = 2\n' > "$FAKEHOME/.config/lsfg-vk/conf.toml"

# ── stubs on PATH ─────────────────────────────────────────────────────────────────────────────
# ★ unshare: RECORD THE ARGV AND ATTEMPT NO REAL MOUNT (B1). The old stub dropped -rm and ran the
# hide script, so `mount --bind /dev/null /dev/dri/<node>` was attempted against the host's real
# render nodes — harmless as a user, but as ROOT it would bind over the host's LIVE GPU nodes. This
# stub reproduces only the STDERR the real path would print when the bind fails (an unprivileged
# user) and runs the command; it never mounts anything.
cat > "$STUB/unshare" <<'EOF'
#!/bin/sh
{ for a in "$@"; do printf 'ARG[%s]\n' "$a"; done; } >> "$STUB_UN"
if [ "${2:-}" = "sh" ] && [ "${3:-}" = "-c" ]; then
    shift 5                 # -rm sh -c SCRIPT sh
    hide="${1:-}"; shift    # the hidden node list
    for n in $hide; do
        [ -e "/dev/dri/$n" ] && \
            echo "  wall: could NOT hide /dev/dri/$n - the game can still see this card" >&2
    done
    exec "$@"
fi
shift                       # e.g. `unshare -Urm true` -> run `true`
exec "$@"
EOF
# ffmpeg: log argv + the LIBVA driver the tool chose for THIS card, then either linger (a SYNTHETIC
# TEST LOAD) or finish at once (a --work job). ★ A JOB NOW WRITES ITS OUTPUT FILE (B3) so the
# on-disk COMPLETE/INCOMPLETE arithmetic and the JOB OUTPUT line are actually exercised, instead of
# every output arriving MISSING.
cat > "$STUB/ffmpeg" <<'EOF'
#!/bin/sh
printf 'FFMPEG ARGS: %s\nLIBVA=%s\n' "$*" "$LIBVA_DRIVER_NAME" >> "$STUB_FF"
case " $* " in *" -f null "*) exec sleep 30 ;; esac   # a test load: stays alive, dies with the game
for last; do :; done                                  # a job: write the file the tool checks on disk
[ -n "${last:-}" ] && : > "$last"
exit 0
EOF
cat > "$STUB/vulkaninfo" <<'EOF'
#!/bin/sh
echo "deviceName stub"
EOF
# ffprobe: the input clip is 120 s; a SPLIT PART reports its own share, so a correct 2-way split sums
# back to the input (COMPLETE) and anything else does not (INCOMPLETE) — B3.
cat > "$STUB/ffprobe" <<'EOF'
#!/bin/sh
case "$*" in
    *_part1.mp4|*_part2.mp4) echo 60.000 ;;
    *)                       echo 120.000 ;;
esac
EOF
# gamescope: --help prints its help to STDERR (it contains "fsr"); otherwise log argv, run the game.
cat > "$STUB/gamescope" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "--help" ]; then
    { echo "gamescope help"; echo "  -F, --filter fsr / nearest / linear"; } >&2
    exit 0
fi
{ for a in "$@"; do printf 'GSARG[%s]\n' "$a"; done; } >> "$STUB_GS"
while [ $# -gt 0 ] && [ "$1" != "--" ]; do shift; done
[ "${1:-}" = "--" ] && shift
exec "$@"
EOF
# steam: exits 0 at once, standing in for a running Steam client that accepts a hand-off and returns.
cat > "$STUB/steam" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$STUB/unshare" "$STUB/ffmpeg" "$STUB/ffprobe" "$STUB/vulkaninfo" "$STUB/gamescope" "$STUB/steam"
ln -sf "$STUB/unshare"    "$NOFFMPEG/unshare"
ln -sf "$STUB/vulkaninfo" "$NOFFMPEG/vulkaninfo"
ln -sf "$STUB/ffmpeg"     "$NOFFPROBE/ffmpeg"
ln -sf "$STUB/unshare"    "$NOFFPROBE/unshare"
ln -sf "$STUB/vulkaninfo" "$NOFFPROBE/vulkaninfo"
ln -sf "$STUB/ffmpeg"     "$NOGS/ffmpeg"
ln -sf "$STUB/unshare"    "$NOGS/unshare"
ln -sf "$STUB/vulkaninfo" "$NOGS/vulkaninfo"
# ★ `exec sleep`: the stub BECOMES sleep, so the SIGKILL on timeout really closes the pipe.
# A non-exec stub (sh spawning sleep) leaves an orphan holding the pipe and costs ~60 s.
printf '#!/bin/sh\nexec sleep 60\n' > "$HANG/gamescope"
chmod +x "$HANG/gamescope"
# ★ NOTOOLS: just enough PATH to launch the interpreter, and NO vulkaninfo/unshare/gamescope, so
# doctor's optional checks all land on UNKNOWN rather than PASS (C).
ln -sf "$(command -v python3)" "$NOTOOLS/python3"
ln -sf "$(command -v sh)"      "$NOTOOLS/sh"

# ★ VKOK: the normal stubs, but a vulkaninfo that HONOURS the exclusive pin like the real one — it
# reads MESA_VK_DEVICE_SELECT=<vid>:<did>! and prints exactly that one device's vendorID/deviceID.
# Without this, doctor's routing-isolation check always saw a count but no id, so no fake fleet could
# ever reach the ALL CHECKS PASSED (exit 0) outcome.
VKOK="$TMP/bin-vkok"; mkdir -p "$VKOK"
cat > "$VKOK/vulkaninfo" <<'EOF'
#!/bin/sh
sel="${MESA_VK_DEVICE_SELECT:-}"
sel="${sel%!}"
vid="${sel%%:*}"; did="${sel#*:}"
echo "deviceName stub GPU"
[ -n "$vid" ] && echo "  vendorID = 0x$vid"
[ -n "$did" ] && echo "  deviceID = 0x$did"
EOF
chmod +x "$VKOK/vulkaninfo"
ln -sf "$STUB/unshare"    "$VKOK/unshare"
ln -sf "$STUB/gamescope"  "$VKOK/gamescope"
ln -sf "$STUB/ffmpeg"     "$VKOK/ffmpeg"
ln -sf "$STUB/ffprobe"    "$VKOK/ffprobe"

# ★ FASTLOAD: a test load that COMPLETES at once (exits 0) instead of lingering, so the tail's
# "COMPLETED its Ns bounded run, as designed" branch is actually exercised.
FASTLOAD="$TMP/bin-fastload"; mkdir -p "$FASTLOAD"
cat > "$FASTLOAD/ffmpeg" <<'EOF'
#!/bin/sh
printf 'FFMPEG ARGS: %s\nLIBVA=%s\n' "$*" "$LIBVA_DRIVER_NAME" >> "$STUB_FF"
case " $* " in *" -f null "*) exit 0 ;; esac   # a test load: completes immediately
for last; do :; done
[ -n "${last:-}" ] && : > "$last"
exit 0
EOF
chmod +x "$FASTLOAD/ffmpeg"
ln -sf "$STUB/unshare"    "$FASTLOAD/unshare"
ln -sf "$STUB/gamescope"  "$FASTLOAD/gamescope"
ln -sf "$STUB/vulkaninfo" "$FASTLOAD/vulkaninfo"
ln -sf "$STUB/ffprobe"    "$FASTLOAD/ffprobe"

# normalizer for the live probe --json (drops the fields that legitimately flap run to run)
NORM_JSON='import sys,json
d=json.load(sys.stdin)
for g in d["gpus"]:
    g.pop("vram_used_mib",None)
    g["link"].pop("current_gen",None)
    g["link"].pop("current_width",None)
    g["link"].pop("gbps_current",None)
    g["link"].pop("idle_degraded",None)
print(json.dumps(d,indent=2,sort_keys=True))'

pass=0; fail=0
DEV=""                   # SPIRIT_DEVICES value for the pair; "" = unset for both children
CHILD_HOME=""            # when non-empty, the CHILD's HOME is replaced (isolate ~/.config writes)
PYBIN="python3"          # the Python interpreter for the pair; overridden to an ABSOLUTE path when
                         # PATH is emptied (an empty PATH cannot find `python3` by name)

kill_stubs() {
  pkill -f "$STUB/unshare"    2>/dev/null
  pkill -f "$STUB/ffmpeg"     2>/dev/null
  pkill -f "$STUB/gamescope"  2>/dev/null
  true
}

# run_side <binary> <args...> — SPIRIT_DEVICES/PATH/HOME/stub logs are granted to the CHILD, never
# exported into this shell (a leaked export poisons every later reading with a fake fleet).
run_side() {
  bin=$1; shift
  if [ -n "$DEV" ] && [ -n "$CHILD_HOME" ]; then
    env SPIRIT_DEVICES="$DEV" HOME="$CHILD_HOME" PATH="$PATHVAL" \
        STUB_UN="$UNLOG" STUB_FF="$FFLOG" STUB_GS="$GSLOG" "$bin" "$@"
  elif [ -n "$DEV" ]; then
    env SPIRIT_DEVICES="$DEV" PATH="$PATHVAL" STUB_UN="$UNLOG" STUB_FF="$FFLOG" STUB_GS="$GSLOG" \
        "$bin" "$@"
  elif [ -n "$CHILD_HOME" ]; then
    env -u SPIRIT_DEVICES HOME="$CHILD_HOME" PATH="$PATHVAL" \
        STUB_UN="$UNLOG" STUB_FF="$FFLOG" STUB_GS="$GSLOG" "$bin" "$@"
  else
    env -u SPIRIT_DEVICES PATH="$PATHVAL" STUB_UN="$UNLOG" STUB_FF="$FFLOG" STUB_GS="$GSLOG" \
        "$bin" "$@"
  fi
}

# compare <label> <SPIRIT_DEVICES-or-empty> <args...>
# Compares exit code, stdout, stderr AND the three stub logs (so the spawn argv and the per-card
# LIBVA driver are checked in every case, not just in the ones that say so).
compare() {
  label=$1; dev=$2; shift 2
  DEV=$dev
  : > "$UNLOG"; : > "$FFLOG"; : > "$GSLOG"
  run_side "$PYBIN" "$PY" "$@" >"$TMP/py.out" 2>"$TMP/py.err"; prc=$?
  cp "$UNLOG" "$TMP/py.un"; cp "$GSLOG" "$TMP/py.gs"; sort "$FFLOG" > "$TMP/py.ff"
  : > "$UNLOG"; : > "$FFLOG"; : > "$GSLOG"
  run_side "$RS" "$@" >"$TMP/rs.out" 2>"$TMP/rs.err"; rrc=$?
  cp "$UNLOG" "$TMP/rs.un"; cp "$GSLOG" "$TMP/rs.gs"; sort "$FFLOG" > "$TMP/rs.ff"
  DEV=""; kill_stubs
  if [ "$prc" != "$rrc" ] || ! diff -q "$TMP/py.out" "$TMP/rs.out" >/dev/null \
                          || ! diff -q "$TMP/py.err" "$TMP/rs.err" >/dev/null \
                          || ! diff -q "$TMP/py.un"  "$TMP/rs.un"  >/dev/null \
                          || ! diff -q "$TMP/py.ff"  "$TMP/rs.ff"  >/dev/null \
                          || ! diff -q "$TMP/py.gs"  "$TMP/rs.gs"  >/dev/null; then
    echo "FAIL: $label   (py rc=$prc rs rc=$rrc)"
    diff -u "$TMP/py.out" "$TMP/rs.out" | head -40
    diff -u "$TMP/py.err" "$TMP/rs.err" | head -40
    diff -u "$TMP/py.un"  "$TMP/rs.un"  | head -20
    diff -u "$TMP/py.ff"  "$TMP/rs.ff"  | head -20
    diff -u "$TMP/py.gs"  "$TMP/rs.gs"  | head -20
    fail=$((fail+1))
  else
    echo "PASS: $label   (rc=$prc)"
    pass=$((pass+1))
  fi
}

echo "== fake-device parity (deterministic) =="
PATHVAL="$STUB:$REAL_PATH"
for wl in 1440p60-framegen 4k60-framegen 1440p-perframe-split; do
  compare "plan $wl" "$FLEET10" plan --workload "$wl"
done
compare "plan llm-13b --vram-mib 8192" "$FLEET10" plan --workload llm-13b --vram-mib 8192
compare "plan unknown workload"        "$FLEET10" plan --workload bogus
compare "probe"                        "$FLEET10" probe
compare "probe --json"                 "$FLEET10" probe --json
compare "roles"                        "$FLEET10" roles
compare "roles --json"                 "$FLEET10" roles --json
compare "roles --profile edit"         "$FLEET10" roles --profile edit
compare "fg 1440p60"                   "$FLEET10" fg --res 1440p60
compare "fg 4k120 round-trip"          "$FLEET10" fg --res 4k120 --mode round-trip
compare "probe (weak-fleet fleet)"     "$FLEETWEAK" probe
compare "probe --json (weak fleet)"    "$FLEETWEAK" probe --json
compare "roles (weak fleet)"           "$FLEETWEAK" roles
compare "roles --json (weak fleet)"    "$FLEETWEAK" roles --json

echo
echo "== --opt=value forms (Python and C++ accept them; Rust alone used to reject: C3) =="
compare "roles --profile=edit"          "$FLEET10" roles --profile=edit
compare "fg --res=1440p60"              "$FLEET10" fg --res=1440p60
compare "fg --res=4k120 --mode=round-trip" "$FLEET10" fg --res=4k120 --mode=round-trip
compare "plan --workload=llm-13b"       "$FLEET10" plan --workload=llm-13b
compare "plan --vram-mib=8192"          "$FLEET10" plan --workload 1440p60-framegen --vram-mib=8192
compare "plan --vram-mib=1_000"         "$FLEET10" plan --vram-mib=1_000
compare "probe --json=1 (argparse error)" "$FLEET10" probe --json=1

echo
echo "== plan --vram-mib: Python's int() trims and ignores underscores (C4) =="
compare "plan --vram-mib ' 12'"         "$FLEET10" plan --vram-mib " 12"
compare "plan --vram-mib 1_000"         "$FLEET10" plan --vram-mib 1_000
compare "plan --vram-mib notanint"      "$FLEET10" plan --vram-mib notanint

echo
echo "== fg --res: the tool's OWN documented examples (A2) =="
compare "fg 4k120"                      "$FLEET10" fg --res 4k120
compare "fg 1080p144"                   "$FLEET10" fg --res 1080p144
compare "fg 1440p@0 (zero -> 60)"       "$FLEET10" fg --res 1440p@0
compare "fg 1440p60@144"                "$FLEET10" fg --res 1440p60@144
compare "fg 4K120 (case-insensitive)"   "$FLEET10" fg --res 4K120
compare "fg unreadable res (note)"      "$FLEET10" fg --res bogus
compare "fg empty res"                  "$FLEET10" fg --res ""

echo
echo "== probe: SPIRIT_DEVICES that cannot be read (B) =="
compare "probe missing fixture -> exit 2"  "$TMP/does-not-exist.json" probe
compare "probe missing fixture (--json)"   "$TMP/does-not-exist.json" probe --json

echo
echo "== boost (v1.0.0: reads the live gamescope / lsfg-vk / sysfs state; same for both sides) =="
compare "boost"                        "$FLEET10" boost
compare "boost (weak fleet)"           "$FLEETWEAK" boost

echo
echo "== run / play (fake command; no game, no real encode) =="
compare "run encode card1 -- echo"     "$FLEET10" run --role encode --card card1 -- /bin/echo HELLO
compare "run compute card6 -- echo"    "$FLEET10" run --role compute --card card6 -- /bin/echo HI --flag -i
compare "run by bdf"                    "$FLEET10" run --role encode --card 0000:02:00.0 -- /bin/echo bybdf
compare "run no such card"              "$FLEET10" run --role encode --card card99 -- /bin/echo x
compare "run missing --role"            "$FLEET10" run --card card1 -- /bin/echo x
compare "run child exit code"           "$FLEET10" run --role encode --card card1 -- /bin/sh -c 'exit 7'
compare "play --no-offload -- echo"    "$FLEET10" play --no-offload -- /bin/echo HELLO
compare "play -- echo (offload stubbed)" "$FLEET10" play -- /bin/echo HELLO

echo
echo "== play: --scale / --framegen argument surface (v1.0.0; exit-2 cases) =="
compare "play --scale 1.5"              "$FLEET10" play --scale 1.5 --no-offload -- /bin/echo HI
compare "play --scale abc"             "$FLEET10" play --scale abc --no-offload -- /bin/echo HI
compare "play --scale (no value)"      "$FLEET10" play --scale
compare "play --framegen 7"            "$FLEET10" play --framegen 7 --no-offload -- /bin/echo HI
compare "play --framegen abc"          "$FLEET10" play --framegen abc --no-offload -- /bin/echo HI
# ★ ISOLATE HOME (B4): on a box where lsfg is INSTALLED this reaches ensure_lsfg_profile, which
# APPENDS to ~/.config/lsfg-vk/conf.toml. The child gets a fake HOME whose conf already names the
# profile, so it takes the read-only "already present" path and the user's REAL config is untouched.
CHILD_HOME="$FAKEHOME"
compare "play --framegen 2"            "$FLEET10" play --framegen 2 --no-offload -- /bin/echo HI
CHILD_HOME=""

echo
echo "== play: the game command that cannot be found (B) and a SIGNALLED game (C2) =="
compare "play missing game cmd (1 card)" "$TMP/one.json" play --no-offload -- /nonexistent/game
compare "play signalled game -> -15"     "$FLEET10" play --no-offload -- /bin/sh -c 'kill -TERM $$'

echo
echo "== play: no GPUs at all (C11) =="
compare "play with no GPUs"             "$TMP/nogpu.json" play -- /bin/echo HI
compare "play --work with no GPUs"      "$TMP/nogpu.json" play --work "$TMP/job.mkv" -- /bin/echo HI

echo
echo "== play: a non-ASCII card name must not panic (C1) =="
compare "non-ASCII name (test load + SKIPPED)" "$TMP/unicode.json" play -- /bin/echo HI
compare "non-ASCII name (--no-offload)"         "$TMP/unicode.json" play --no-offload -- /bin/echo HI

echo
echo "== play --work: A REAL JOB, and the three reasons none is started (A1/A6) =="
compare "play --work (replication)"     "$FLEET10" play --work "$TMP/job.mkv" -- /bin/echo HELLO
compare "play --work --split"           "$FLEET10" play --work "$TMP/job.mkv" --split -- /bin/echo HELLO
compare "play --work-outdir with .."   "$FLEET10" play --work "$TMP/job.mkv" --work-outdir "$TMP/out/../jobs" -- /bin/echo HELLO
compare "play --work non-file"          "$FLEET10" play --work "$TMP/nonfile-dir" -- /bin/echo HELLO
compare "play --no-offload --work"      "$FLEET10" play --no-offload --work "$TMP/job.mkv" -- /bin/echo HELLO
PATHVAL="$NOFFMPEG:$REAL_PATH"
compare "play --work with no ffmpeg"    "$FLEET10" play --work "$TMP/job.mkv" -- /bin/echo HELLO
PATHVAL="$STUB:$REAL_PATH"
compare "play --work, no card can encode" "$TMP/noenc.json" play --work "$TMP/job.mkv" -- /bin/echo HELLO
PATHVAL="$NOFFPROBE:$REAL_PATH"
compare "play --split, duration unreadable (C8)" "$FLEET10" play --work "$TMP/job.mkv" --split -- /bin/echo HELLO
PATHVAL="$STUB:$REAL_PATH"
compare "play --split, one card only (C9 wording)" "$TMP/two.json" play --work "$TMP/job.mkv" --split -- /bin/echo HI

echo
echo "== play --spare: dedupe, the unknown-node notice, the REFUSED wording (C6) =="
compare "play --spare renderD130"       "$FLEET10" play --spare renderD130 -- /bin/echo HELLO
compare "play --spare renderD130,renderD130 (dedupe)" "$FLEET10" play --spare renderD130,renderD130 -- /bin/echo HELLO
compare "play --spare unknown node"     "$FLEET10" play --spare renderD999 -- /bin/echo HELLO
compare "play --spare known,unknown"    "$FLEET10" play --spare renderD130,renderD999 -- /bin/echo HELLO
compare "play --spare (no value)"       "$FLEET10" play --spare

echo
echo "== play --overlay: the refusal happens BEFORE anything is spawned (A5) =="
compare "play --overlay (gamescope READY)" "$FLEET10" play --overlay -- /bin/echo HI
CHILD_HOME="$FAKEHOME"
compare "play --overlay --framegen 2"      "$FLEET10" play --overlay --framegen 2 -- /bin/echo HI
CHILD_HOME=""
PATHVAL="$NOGS:$REAL_PATH"
compare "play --overlay, gamescope absent" "$FLEET10" play --overlay -- /bin/echo HI
PATHVAL="$STUB:$REAL_PATH"

echo
echo "== play: the Steam path (blocking item 2 — said, not silent) =="
# ★ A STEAM LAUNCH CANNOT BE PINNED FROM HERE, AND BOTH BUILDS MUST SAY SO. Two claims, two cases:
#   (1) `steam://rungameid/N` is not a program: both must print the launch-options RECIPE and the same
#       exec-failure line (rc 127).
#   (2) a launcher that RETURNS AT ONCE (`steam`, stubbed to exit 0) must be named as having routed
#       NOTHING — the old code printed `game exited (rc=0)` as if the session had worked.
# The recipe line carries each build's OWN absolute path, and the fast-return line a measured time;
# normalize both — the CLAIM is the message, not the path or the clock.
_steam_norm() {
    sed -e 's#".*" play -- %command%#"<SELF>" play -- %command%#' \
        -e 's/after [0-9][0-9.]*s/after N.Ns/'
}
compare_steam() {
  label=$1; dev=$2; shift 2
  DEV=$dev
  run_side "$PYBIN" "$PY" "$@" >"$TMP/py.raw" 2>/dev/null; prc=$?
  run_side "$RS" "$@" >"$TMP/rs.raw" 2>/dev/null; rrc=$?
  _steam_norm < "$TMP/py.raw" > "$TMP/py.out"
  _steam_norm < "$TMP/rs.raw" > "$TMP/rs.out"
  DEV=""; kill_stubs
  if [ "$prc" != "$rrc" ] || ! diff -q "$TMP/py.out" "$TMP/rs.out" >/dev/null; then
    echo "FAIL: $label   (py rc=$prc rs rc=$rrc)"; fail=$((fail+1))
    diff -u "$TMP/py.out" "$TMP/rs.out" | head -30
  else echo "PASS: $label   (rc=$prc)"; pass=$((pass+1)); fi
}
# isolate HOME so the session-pin state is OFF regardless of what this dev box has enabled
PINOFF="$TMP/pinoff"; mkdir -p "$PINOFF"; CHILD_HOME="$PINOFF"
compare_steam "play -- steam:// (says it cannot be pinned; prints the recipe)" \
        "$FLEET10" play --no-offload -- steam://rungameid/123
compare_steam "play -- steam (fast clean exit -> session did NOT route the game)" \
        "$FLEET10" play --no-offload -- steam
if grep -q 'play -- %command%' "$TMP/rs.out" && grep -q 'did NOT route the game' "$TMP/rs.out"; then
  echo "PASS: steam message present (recipe + not-routed)"; pass=$((pass+1))
else echo "FAIL: steam message missing from the output"; fail=$((fail+1)); cat "$TMP/rs.out"; fi
echo
echo "== play: the Steam path with the session-wide pin ON (spirit enable) =="
PINON="$TMP/pinon"; mkdir -p "$PINON/.config/environment.d"
printf 'MESA_VK_DEVICE_SELECT=1002:1234!\nDRI_PRIME=pci-0000:01:00.0\n' \
    > "$PINON/.config/environment.d/90-spirit.conf"
CHILD_HOME="$PINON"
compare_steam "play -- steam with the pin ON (says it is already pinned)" \
        "$FLEET10" play --no-offload -- steam
CHILD_HOME=""
if grep -q 'session-wide pin IS on' "$TMP/rs.out"; then
  echo "PASS: steam pin-ON message present"; pass=$((pass+1))
else echo "FAIL: steam pin-ON message missing"; fail=$((fail+1)); cat "$TMP/rs.out"; fi

echo
echo "== enable / disable: the session-wide pin (isolated HOME, no side effects) =="
# The pin lives in $HOME/.config/environment.d — isolated here so a test never touches the real one and
# both sides start from the same state. `--now` is NOT run (it would set the user manager's env).
ENHOME="$TMP/enhome"; mkdir -p "$ENHOME"; CHILD_HOME="$ENHOME"
compare "enable writes the session-wide pin" "$FLEET10" enable
en_reset() { mkdir -p "$ENHOME/.config/environment.d"
             printf 'MESA_VK_DEVICE_SELECT=1002:1234!\nDRI_PRIME=pci-0000:01:00.0\n' \
                 > "$ENHOME/.config/environment.d/90-spirit.conf"; }
en_reset; run_side "$PYBIN" "$PY" disable >"$TMP/py.out" 2>"$TMP/py.err"; prc=$?
en_reset; run_side "$RS" disable >"$TMP/rs.out" 2>"$TMP/rs.err"; rrc=$?
if [ "$prc" = "$rrc" ] && diff -q "$TMP/py.out" "$TMP/rs.out" >/dev/null 2>&1; then
  echo "PASS: disable removes the pin   (rc=$prc)"; pass=$((pass+1))
else echo "FAIL: disable removes the pin   (py rc=$prc rs rc=$rrc)"; fail=$((fail+1))
     diff -u "$TMP/py.out" "$TMP/rs.out" | head -20; fi
compare "disable when it was never on" "$FLEET10" disable
compare "enable --bogus (exit 2, our flag only)" "$FLEET10" enable --bogus
CHILD_HOME=""

echo
echo "== C5: a hung child must not hang THIS tool (gamescope --help, 15 s each side) =="
# ★ WIRE THE HANG DIRECTORY ONTO PATHVAL (B3): it was built but never used, so the case never hung.
PATHVAL="$HANG:$REAL_PATH"
compare "boost with a hung gamescope"  "$FLEET10" boost
PATHVAL="$STUB:$REAL_PATH"

echo
echo "== play: gamescope wrapping, with a readable native mode (argv compared via the stub) =="
compare "play --scale 0.7 (gamescope wrapped)" "$TMP/modes.json" play --scale 0.7 -- /bin/echo HI
CHILD_HOME="$FAKEHOME"
compare "play --scale 0.7 --framegen 2"        "$TMP/modes.json" play --scale 0.7 --framegen 2 -- /bin/echo HI
compare "play --overlay --framegen 2 (native)" "$TMP/modes.json" play --overlay --framegen 2 -- /bin/echo HI
CHILD_HOME=""
compare "play --overlay (native readable)"     "$TMP/modes.json" play --overlay -- /bin/echo HI

echo
echo "== C: the play DEFAULT — nothing runs without --work/--test-load (A4) =="
compare "play default (IDLE tail, nothing starts)"    "$FLEET10"   play -- /bin/echo HI
compare "play default (weak fleet)"                    "$FLEETWEAK" play -- /bin/echo HI
compare "play default --no-offload"                    "$FLEET10"   play --no-offload -- /bin/echo HI
compare "play -- ./game --version LAUNCHES (A1)"       "$FLEET10"   play --no-offload -- /bin/echo --version
compare "play -- ./game --json still prints the mark"  "$FLEET10"   play --no-offload -- /bin/echo --json

echo
echo "== C: --test-load [SECS] (A4) =="
compare "play --test-load 5 (bounded load)"   "$TMP/two.json" play --test-load 5 -- /bin/echo HI
compare "play --test-load=30 (= form)"        "$TMP/two.json" play --test-load=30 -- /bin/echo HI
compare "play --test-load (default 60)"       "$TMP/two.json" play --test-load -- /bin/echo HI
compare "play --test-load=abc (exit 2)"       "$FLEET10"      play --test-load=abc -- /bin/echo HI
compare "play --test-load= (empty, exit 2)"   "$FLEET10"      play --test-load= -- /bin/echo HI
# ★ THE TWO NOW-DISTINCT REPORTING PATHS (/2.5): a load that COMPLETES its bounded run
# (rc 0 → "COMPLETED its Ns bounded run, as designed") vs one still running when the game exits
# (bounded by the parent-side wall clock).
PATHVAL="$FASTLOAD:$REAL_PATH"
compare "play --test-load 6 (load COMPLETES the run)"     "$TMP/two.json" play --test-load 6 -- /bin/sh -c 'sleep 1'
PATHVAL="$STUB:$REAL_PATH"
compare "play --test-load 5 (load capped, still running)" "$TMP/two.json" play --test-load 5 -- /bin/echo HI

echo
echo "== C: the MARGINAL rule and its --spare override (A4) =="
compare "play --work marginal exclusion (NOT using)"  "$FLEET10" play --work "$TMP/job.mkv" -- /bin/echo HI
compare "play --spare override includes a marginal"   "$FLEET10" play --spare renderD129 --work "$TMP/job.mkv" -- /bin/echo HI

echo
echo "== C: plan / fg additions (A2/A3) =="
compare "plan --vram-mib 6898 (A3 figures)"   "$FLEET10" plan --workload 1440p60-framegen --vram-mib 6898
compare "fg --res 1440p60 (link bound)"       "$FLEET10" fg --res 1440p60

echo
echo "== C: a real SPLIT — COMPLETE (2 parts sum back) vs INCOMPLETE (3 do not) (B3) =="
compare "play --split 2 cards -> COMPLETE"    "$TMP/split2.json" play --work "$TMP/job.mkv" --split -- /bin/echo HI
compare "play --split 3 cards -> INCOMPLETE"  "$TMP/split3.json" play --work "$TMP/job.mkv" --split -- /bin/echo HI
compare "play --work replication writes output" "$TMP/split2.json" play --work "$TMP/job.mkv" -- /bin/echo HI
compare "play --work non-file -> REFUSED (B3)" "$FLEET10" play --work "$TMP/nonfile-dir" -- /bin/echo HELLO

echo
echo "== C: doctor in its THREE outcome states — exit 0 / 1 / 3 (A5) =="
# (0) ALL CHECKS PASSED: distinct cards + a vulkaninfo that HONOURS the pin, so routing-isolation
#     PASSES (a bare count with no matching id could only ever FAIL or be UNKNOWN).
PATHVAL="$VKOK:$REAL_PATH"
compare "doctor ALL CHECKS PASSED (exit 0)"    "$TMP/doctorok.json" doctor
PATHVAL="$STUB:$REAL_PATH"
# (1) PROBLEMS FOUND: two cards share vendor:device, so the pin cannot tell them apart.
compare "doctor duplicate vid:did (exit 1)"    "$TMP/dup.json"     doctor
# (3) NOT VERIFIED: no vulkaninfo/unshare/gamescope on PATH, so the device pin (a CORE check) cannot
#     be tested at all. ★ The interpreter is named by ABSOLUTE PATH, because with an EMPTY PATH
#     `python3` is not resolvable by name.
PYBIN="$(command -v python3)"
PATHVAL=""
compare "doctor tools-absent (empty PATH; exit 3)" "$TMP/onecard.json" doctor
PATHVAL="$STUB:$REAL_PATH"
PYBIN="python3"

echo
echo "== doctor (fake fleet; uses real vulkaninfo/gamescope/lsfg, identical on both sides) =="
compare "doctor (fleet-10)"             "$FLEET10"   doctor

echo
echo "== help / usage surface =="
compare "no args"                       ""
compare "empty subcommand arg"          ""          ""
compare "top-level --help"              ""          --help
compare "--version (one line, exit 0)"  ""          --version
for c in probe roles plan doctor fg boost; do compare "$c -h" "" "$c" -h; done
compare "origin -h (C10)"               ""          origin -h
compare "origin --bogus (C10)"          ""          origin --bogus
compare "origin --check"                ""          origin --check
compare "origin"                        ""          origin
compare "ui -h"                         ""          ui -h
compare "ui --help"                     ""          ui --help
compare "play --help"                    ""          play --help
compare "play -h"                       ""          play -h
compare "unknown subcommand"            ""          frobnicate
compare "unrecognized arg (probe)"      ""          probe --bogus
# ★ ABBREVIATIONS MUST BE REJECTED (added 2026-10-11). argparse's allow_abbrev does NOT propagate to
# subparsers, so Python used to accept `probe --js` while both ports rejected it. No case could exist
# until the reference was repaired; now one can.
compare "probe --js (abbrev rejected)"   ""          probe --js
compare "roles --profil (abbrev)"        ""          roles --profil edit
compare "fg --re (abbrev rejected)"      ""          fg --re 4k120
compare "unrecognized trailing"         ""          probe --json extra
compare "unrecognized arg (boost)"      ""          boost --bogus
compare "missing value (roles)"         ""          roles --profile
compare "bad --mode"                    ""          fg --mode sideways
compare "bad --vram-mib"                ""          plan --vram-mib notanint

# ── PHASE 2: the live box. No stubs on PATH — the real sysfs, vulkaninfo, gamescope and lsfg
# state are read. No `play` here on purpose: with the real `unshare` present a play case would
# spawn a real mount namespace on this machine, which is exactly what the stubs exist to avoid.
echo
echo "== real box (SPIRIT_DEVICES unset, no stubs on PATH) =="
PATHVAL="$REAL_PATH"
compare "roles (real)"                  ""  roles
compare "roles --json (real)"           ""  roles --json
compare "plan framegen (real)"          ""  plan --workload 1440p60-framegen
compare "plan vram (real)"              ""  plan --workload 1440p60-framegen --vram-mib 6898
compare "fg 4k120 (real)"               ""  fg --res 4k120
compare "doctor (real)"                ""  doctor
compare "boost (real)"                 ""  boost

# ★ THE LIVE PROBE FLAPS: the "[idle now: xN GenN]" suffix comes and goes with the card's own
# low-power link state between two invocations milliseconds apart. It is a measurement of the
# MACHINE, not a property of a build — so it is stripped here, exactly as the --json comparison
# below strips the same field. ★ And the line's trailing padding goes with it: the suffix is the
# last thing on the line, so on a run where only ONE side saw it the `%-6s` padding of the `out`
# column is left behind on the other and the two lines differ by whitespace alone. A suite that
# fails at random is not evidence of anything, so both are normalized.
DEV=""
PROBE_NORM='s/\[idle now: [^]]*\]//; s/[[:space:]]*$//'
run_side python3 "$PY" probe 2>/dev/null | sed "$PROBE_NORM" > "$TMP/py.out"
run_side "$RS"        probe 2>/dev/null | sed "$PROBE_NORM" > "$TMP/rs.out"
if diff -q "$TMP/py.out" "$TMP/rs.out" >/dev/null; then
  echo "PASS: probe (real; live link suffix normalized)"; pass=$((pass+1))
else echo "FAIL: probe (real)"; fail=$((fail+1)); diff -u "$TMP/py.out" "$TMP/rs.out" | head -40; fi

run_side python3 "$PY" probe --json 2>/dev/null | python3 -c "$NORM_JSON" > "$TMP/py.out"
run_side "$RS"        probe --json 2>/dev/null | python3 -c "$NORM_JSON" > "$TMP/rs.out"
if diff -q "$TMP/py.out" "$TMP/rs.out" >/dev/null; then
  echo "PASS: probe --json (real; unstable fields stripped)"; pass=$((pass+1))
else echo "FAIL: probe --json (real)"; fail=$((fail+1)); diff -u "$TMP/py.out" "$TMP/rs.out" | head -40; fi

echo
echo "== summary: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
