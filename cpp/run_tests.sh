#!/bin/sh
# run_tests.sh — differential test: the C++ port (./spirit) vs the Python original (../spirit.py).
#
# Runs both binaries with an identical environment and diffs stdout/stderr/exit codes in two modes:
#   FAKE  — SPIRIT_DEVICES = fleet-10.example.json   (deterministic, 10 GPUs)
#   REAL  — SPIRIT_DEVICES unset                     (the live 4-GPU box)
#
# SPIRIT_DEVICES is passed to each CHILD via `env`, NEVER exported into this shell's environment
# (a leaked export poisons every later reading with a fake fleet).
#
# unshare / ffmpeg / vulkaninfo / gamescope are stubbed on PATH so `run` and `play` can be verified
# (env set, argv spawned) without touching a real GPU or a real display. Nothing is written outside
# $TMPDIR.  In REAL mode the gamescope stub is removed so the live binary is exercised.
#
# Usage:  ./run_tests.sh          (build first: make)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
PYSPIRIT=${PYSPIRIT:-$HERE/../spirit.py}
PYTHON=${PYTHON:-python3}
CPP="$HERE/spirit"
FLEET="$HERE/../fleet-10.example.json"

[ -x "$CPP" ]     || { echo "build first: make" >&2; exit 1; }
[ -f "$PYSPIRIT" ] || { echo "spirit.py not found at $PYSPIRIT" >&2; exit 1; }

WORK=$(mktemp -d)
STUB="$WORK/bin"; mkdir -p "$STUB"
export STUB_UN="$WORK/un.log" STUB_FF="$WORK/ff.log" STUB_GS="$WORK/gs.log"
pass=0; fail=0
FAKE=""                 # set to the fleet path for PHASE 1; the child sees it, this shell does not.

# ── stubs on PATH ─────────────────────────────────────────────────────────────
cat > "$STUB/unshare" <<'EOF'
#!/bin/sh
{ for a in "$@"; do printf 'ARG[%s]\n' "$a"; done; } >> "$STUB_UN"
shift           # drop -rm; then act like: sh -c SCRIPT sh HIDESTR cmd...
exec "$@"
EOF
cat > "$STUB/ffmpeg" <<'EOF'
#!/bin/sh
printf 'FFMPEG ARGS: %s\nLIBVA=%s\n' "$*" "$LIBVA_DRIVER_NAME" >> "$STUB_FF"
exec sleep 30
EOF
cat > "$STUB/vulkaninfo" <<'EOF'
#!/bin/sh
echo "deviceName stub"
EOF
# gamescope: --help prints its help to STDERR (contains "fsr"); otherwise log argv and run the game.
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
chmod +x "$STUB/unshare" "$STUB/ffmpeg" "$STUB/vulkaninfo" "$STUB/gamescope"

# normalizer applied to the live probe --json (drops fields that legitimately flap)
NORM_JSON='import sys,json
d=json.load(sys.stdin)
for g in d["gpus"]:
    g.pop("vram_used_mib",None)
    g["link"].pop("current_gen",None)
    g["link"].pop("current_width",None)
    g["link"].pop("gbps_current",None)
    g["link"].pop("idle_degraded",None)
print(json.dumps(d,indent=2,sort_keys=True))'

# SPIRIT_DEVICES is granted to the child, never exported into this shell.
run_py()  { if [ -n "$FAKE" ]; then env SPIRIT_DEVICES="$FAKE" timeout 60 "$PYTHON" "$PYSPIRIT" "$@";
            else timeout 60 "$PYTHON" "$PYSPIRIT" "$@"; fi; }
run_cpp() { if [ -n "$FAKE" ]; then env SPIRIT_DEVICES="$FAKE" timeout 60 "$CPP" "$@";
            else timeout 60 "$CPP" "$@"; fi; }

cmp_cmd() { # label args...
    label=$1; shift
    run_py  "$@" > "$WORK/py.out" 2>"$WORK/py.err"; pyrc=$?
    run_cpp "$@" > "$WORK/cpp.out" 2>"$WORK/cpp.err"; crc=$?
    if [ "$pyrc" != "$crc" ]; then
        echo "FAIL  $label   (exit py=$pyrc cpp=$crc)"; fail=$((fail+1));
        diff "$WORK/py.out" "$WORK/cpp.out" | head -30; return
    fi
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null 2>&1 && \
       diff -q "$WORK/py.err" "$WORK/cpp.err" >/dev/null 2>&1; then
        echo "PASS  $label"; pass=$((pass+1))
    else
        echo "FAIL  $label"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out" | head -30
        diff "$WORK/py.err" "$WORK/cpp.err" | head -10
    fi
}

# stdout-only compare (used where a real child writes volatile logs to stderr, e.g. gamescope)
cmp_stdout() {
    label=$1; shift
    run_py  "$@" > "$WORK/py.out" 2>"$WORK/py.err"; pyrc=$?
    run_cpp "$@" > "$WORK/cpp.out" 2>"$WORK/cpp.err"; crc=$?
    if [ "$pyrc" != "$crc" ]; then echo "FAIL  $label (exit py=$pyrc cpp=$crc)"; fail=$((fail+1)); return; fi
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then
        echo "PASS  $label (stdout; stderr = game/gamescope child logs)"; pass=$((pass+1))
    else echo "FAIL  $label"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out" | head -30; fi
}

# ★ THE LIVE PROBE FLAPS, AND IT MADE THIS SUITE FLAKY (fix 2026-10-11). One field — the
# "[idle now: x16 GenN]" suffix — comes and goes with the card's own low-power link state between two
# invocations milliseconds apart. It is a live measurement of the MACHINE, not a property of a build,
# and the --json comparison below already strips the same field (idle_degraded) for exactly this
# reason. Unstripped, this single case failed roughly one run in four — and a suite that fails at
# random is not evidence of anything. Strip it here too.
cmp_probe_real() {
    label=$1; shift
    run_py  "$@" 2>/dev/null | sed 's/ *\[idle now: [^]]*\]//' > "$WORK/py.out"
    run_cpp "$@" 2>/dev/null | sed 's/ *\[idle now: [^]]*\]//' > "$WORK/cpp.out"
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then
        echo "PASS  $label (live link suffix normalized)"; pass=$((pass+1))
    else
        echo "FAIL  $label"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out" | head -30
    fi
}

# ── PHASE 1: deterministic fake fleet ─────────────────────────────────────────
echo "=== PHASE 1: SPIRIT_DEVICES = fleet-10.example.json  (deterministic, child-only) ==="
FAKE="$FLEET"
export PATH="$STUB:$PATH"
cmp_cmd "probe"                 probe
cmp_cmd "probe --json"          probe --json
cmp_cmd "roles"                 roles
cmp_cmd "roles --json"          roles --json
cmp_cmd "roles --profile edit"  roles --profile edit
cmp_cmd "plan 1440p60-framegen" plan --workload 1440p60-framegen
cmp_cmd "plan 4k60-framegen"    plan --workload 4k60-framegen
cmp_cmd "plan split"            plan --workload 1440p-perframe-split
cmp_cmd "plan llm-13b"          plan --workload llm-13b
cmp_cmd "plan --vram-mib 6898"  plan --workload 1440p60-framegen --vram-mib 6898
cmp_cmd "plan unknown"          plan --workload nope
cmp_cmd "fg 1440p60"            fg --res 1440p60
cmp_cmd "fg 1440p60 round-trip" fg --res 1440p60 --mode round-trip
cmp_cmd "fg 4k120"              fg --res 4k120
cmp_cmd "doctor"                doctor
cmp_cmd "boost"                 boost
cmp_cmd "no-args help"
cmp_cmd "run (encode, no cmd)"  run --role encode --card card2
# argparse help / error paths
cmp_cmd "probe -h"              probe -h
cmp_cmd "roles -h"              roles -h
cmp_cmd "plan -h"               plan -h
cmp_cmd "doctor -h"             doctor -h
cmp_cmd "boost -h"              boost -h
cmp_cmd "fg -h"                 fg -h
cmp_cmd "probe --foo"           probe --foo
cmp_cmd "doctor -x"             doctor -x
cmp_cmd "boost --foo"           boost --foo
cmp_cmd "plan foo"              plan foo
cmp_cmd "plan --workload"       plan --workload
cmp_cmd "plan --vram-mib abc"   plan --vram-mib abc
cmp_cmd "fg --mode bogus"       fg --mode bogus
cmp_cmd "roles --profile=edit"  roles --profile=edit
# play --scale / --framegen flag validation (hand-parsed; exit 2)
cmp_cmd "play --scale 1.5"      play --scale 1.5 -- /bin/echo HI
cmp_cmd "play --framegen 7"     play --framegen 7 -- /bin/echo HI
cmp_cmd "play --scale abc"      play --scale abc -- /bin/echo HI
cmp_cmd "play --scale (novalue)" play --scale
cmp_cmd "play (no cmd)"         play
cmp_cmd "ui -h"                 ui -h
cmp_cmd "ui --help"             ui --help
cmp_cmd "play --help"           play --help
cmp_cmd "play -h"               play -h

echo "--- run: the environment it sets (child = env) ---"
for card in card0 card3 card6 card8; do
    run_py  run --role encode --card $card -- env 2>/dev/null | grep -E '^(DRI_PRIME|MESA_VK_DEVICE_SELECT|SPIRIT_VAAPI_DEVICE|GGML_VK_VISIBLE_DEVICES)=' > "$WORK/py.env"
    run_cpp run --role encode --card $card -- env 2>/dev/null | grep -E '^(DRI_PRIME|MESA_VK_DEVICE_SELECT|SPIRIT_VAAPI_DEVICE|GGML_VK_VISIBLE_DEVICES)=' > "$WORK/cpp.env"
    if diff -q "$WORK/py.env" "$WORK/cpp.env" >/dev/null 2>&1 && [ -s "$WORK/cpp.env" ]; then
        echo "PASS  run env --role encode --card $card"; pass=$((pass+1))
        sed 's/^/          /' "$WORK/cpp.env"
    else echo "FAIL  run env --card $card"; fail=$((fail+1)); diff "$WORK/py.env" "$WORK/cpp.env"; fi
done
run_py  run --role compute --card card1 -- env 2>/dev/null | grep -E '^(DRI_PRIME|MESA_VK_DEVICE_SELECT|GGML_VK_VISIBLE_DEVICES)=' > "$WORK/py.env"
run_cpp run --role compute --card card1 -- env 2>/dev/null | grep -E '^(DRI_PRIME|MESA_VK_DEVICE_SELECT|GGML_VK_VISIBLE_DEVICES)=' > "$WORK/cpp.env"
if diff -q "$WORK/py.env" "$WORK/cpp.env" >/dev/null 2>&1 && [ -s "$WORK/cpp.env" ]; then
    echo "PASS  run env --role compute --card card1"; pass=$((pass+1))
    sed 's/^/          /' "$WORK/cpp.env"
else echo "FAIL  run env compute"; fail=$((fail+1)); diff "$WORK/py.env" "$WORK/cpp.env"; fi

run_py  run --role encode --card card99 >"$WORK/py.out" 2>"$WORK/py.err"; pyrc=$?
run_cpp run --role encode --card card99 >"$WORK/cpp.out" 2>"$WORK/cpp.err"; crc=$?
if [ "$pyrc" = "$crc" ] && diff -q "$WORK/py.err" "$WORK/cpp.err" >/dev/null; then
    echo "PASS  run bad-card exit=$crc + stderr"; pass=$((pass+1))
else echo "FAIL  run bad card (py=$pyrc cpp=$crc)"; fail=$((fail+1)); diff "$WORK/py.err" "$WORK/cpp.err"; fi

echo "--- play: stdout + exact spawn argv (offload stubbed) ---"
for o in "--offload" "--no-offload"; do
    arg=""; [ "$o" = "--no-offload" ] && arg="--no-offload"
    : > "$STUB_UN"; : > "$STUB_FF"
    run_py  play $arg -- /bin/echo HELLO > "$WORK/py.out" 2>&1
    cp "$STUB_UN" "$WORK/py.un"; cp "$STUB_FF" "$WORK/py.ff"
    : > "$STUB_UN"; : > "$STUB_FF"
    run_cpp play $arg -- /bin/echo HELLO > "$WORK/cpp.out" 2>&1
    cp "$STUB_UN" "$WORK/cpp.un"; cp "$STUB_FF" "$WORK/cpp.ff"
    lbl="play $o -- /bin/echo"
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then echo "PASS  $lbl stdout"; pass=$((pass+1))
    else echo "FAIL  $lbl stdout"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out"|head -20; fi
    if diff -q "$WORK/py.un" "$WORK/cpp.un" >/dev/null 2>&1; then echo "PASS  $lbl unshare-argv"; pass=$((pass+1))
    else echo "FAIL  $lbl unshare-argv"; fail=$((fail+1)); diff "$WORK/py.un" "$WORK/cpp.un"|head -30; fi
    if [ "$o" = "--offload" ]; then
        sort "$WORK/py.ff" > "$WORK/py.ffs"; sort "$WORK/cpp.ff" > "$WORK/cpp.ffs"
        if [ -s "$WORK/cpp.ffs" ] && diff -q "$WORK/py.ffs" "$WORK/cpp.ffs" >/dev/null; then
            echo "PASS  $lbl ffmpeg argv ($(grep -c '^FFMPEG' "$WORK/cpp.ff") encodes)"; pass=$((pass+1))
        else echo "FAIL  $lbl ffmpeg argv"; fail=$((fail+1)); diff "$WORK/py.ffs" "$WORK/cpp.ffs"|head -30; fi
    fi
    pkill -f "$STUB/unshare" 2>/dev/null; pkill -f "$STUB/ffmpeg" 2>/dev/null; pkill -f "$STUB/gamescope" 2>/dev/null
done

echo "--- play --scale / --framegen: stdout + exact gamescope argv (gamescope stubbed) ---"
for combo in "--scale 0.7 --no-offload" "--framegen 2 --no-offload" "--scale 0.7 --framegen 2 --no-offload"; do
    : > "$STUB_UN"; : > "$STUB_FF"; : > "$STUB_GS"
    run_py  play $combo -- /bin/echo HELLO > "$WORK/py.out" 2>&1
    cp "$STUB_GS" "$WORK/py.gs"; cp "$STUB_UN" "$WORK/py.un"; cp "$STUB_FF" "$WORK/py.ff"
    : > "$STUB_UN"; : > "$STUB_FF"; : > "$STUB_GS"
    run_cpp play $combo -- /bin/echo HELLO > "$WORK/cpp.out" 2>&1
    cp "$STUB_GS" "$WORK/cpp.gs"; cp "$STUB_UN" "$WORK/cpp.un"; cp "$STUB_FF" "$WORK/cpp.ff"
    lbl="play $combo"
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then echo "PASS  $lbl stdout"; pass=$((pass+1))
    else echo "FAIL  $lbl stdout"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out"|head -20; fi
    if diff -q "$WORK/py.gs" "$WORK/cpp.gs" >/dev/null 2>&1; then echo "PASS  $lbl gamescope-argv"
    else echo "FAIL  $lbl gamescope-argv"; fail=$((fail+1)); diff "$WORK/py.gs" "$WORK/cpp.gs"|head -30; fi
    pkill -f "$STUB/unshare" 2>/dev/null; pkill -f "$STUB/ffmpeg" 2>/dev/null; pkill -f "$STUB/gamescope" 2>/dev/null
done

# ── PHASE 2: the live box (real binaries; gamescope stub removed) ─────────────
echo
echo "=== PHASE 2: real box (SPIRIT_DEVICES unset, no stubs on PATH) ==="
FAKE=""
export PATH="${PATH#"$STUB:"}"
cmp_probe_real "probe (real)"    probe
cmp_cmd "roles (real)"           roles
cmp_cmd "roles --json (real)"    roles --json
cmp_cmd "plan framegen (real)"   plan --workload 1440p60-framegen
cmp_cmd "plan vram 6898 (real)"  plan --workload 1440p60-framegen --vram-mib 6898
cmp_cmd "fg (real)"              fg --res 1440p60
cmp_cmd "doctor (real)"          doctor
cmp_cmd "boost (real)"           boost
cmp_cmd "play --framegen 2 (real)" play --framegen 2 --no-offload -- /bin/echo HI
cmp_cmd "play --scale 1.5 (real)"  play --scale 1.5 -- /bin/echo HI
cmp_cmd "play --framegen 7 (real)" play --framegen 7 -- /bin/echo HI
cmp_cmd "play --scale abc (real)"  play --scale abc -- /bin/echo HI
cmp_stdout "play --scale 0.7 (real)" play --scale 0.7 --no-offload -- /bin/echo HI

run_py  probe --json 2>/dev/null | "$PYTHON" -c "$NORM_JSON" > "$WORK/py.out"
run_cpp probe --json 2>/dev/null | "$PYTHON" -c "$NORM_JSON" > "$WORK/cpp.out"
if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then
    echo "PASS  probe --json (real, unstable fields stripped)"; pass=$((pass+1))
else echo "FAIL  probe --json (real)"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out"|head -40; fi

pkill -f "$STUB/unshare" 2>/dev/null; pkill -f "$STUB/ffmpeg" 2>/dev/null; pkill -f "$STUB/gamescope" 2>/dev/null
rm -rf "$WORK"
echo
echo "================  $pass passed, $fail failed  ================"
[ "$fail" = 0 ]
