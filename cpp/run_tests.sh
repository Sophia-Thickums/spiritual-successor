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
# unshare / ffmpeg / ffprobe / vulkaninfo / gamescope are stubbed on PATH so `run` and `play` can be
# verified (env set, argv spawned) without touching a real GPU or a real display. Nothing is written
# outside $TMPDIR.  In REAL mode the gamescope stub is removed so the live binary is exercised.
#
# ★ COVERAGE ADDED 2026-10-11 (outside review): the suite had NO case for --work, --split,
# --work-outdir, --spare, --overlay, origin, a Wine/.exe target, a missing game command, a signalled
# game, an early-dying test load, or a non-ASCII card name — which is exactly why the divergences in
# the port spec were invisible. Those cases now exist below, all deterministic.
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
CHILD_PATH=""           # when non-empty, the CHILD's PATH is replaced (special stub sets)
CHILD_HOME=""           # when non-empty, the CHILD's HOME is replaced (isolate ~/.config writes)

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
cat > "$STUB/ffprobe" <<'EOF'
#!/bin/sh
echo "60.000"
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
chmod +x "$STUB/unshare" "$STUB/ffmpeg" "$STUB/ffprobe" "$STUB/vulkaninfo" "$STUB/gamescope"

# ── auxiliary stub sets (★ new coverage) ──────────────────────────────────────
# FASTB: same stubs but ffmpeg exits at once, so a --work job finishes the moment the game does
#        (the default ffmpeg sleeps 30 s, which would make every job case a 30 s wait).
FASTB="$WORK/fastbin"; mkdir -p "$FASTB"
cp "$STUB/unshare" "$STUB/ffprobe" "$STUB/vulkaninfo" "$STUB/gamescope" "$FASTB/"
cat > "$FASTB/ffmpeg" <<'EOF'
#!/bin/sh
# no output is produced — this is what the on-disk check must report
exit 0
EOF
chmod +x "$FASTB"/*
# MINB: only what the harness needs to launch the interpreter and the game (no unshare, no
#       gamescope, at all). Used for the cases that must observe a MISSING tool: the overlay refusal
#       (gamescope absent) and the game command that cannot be found (no unshare to wrap it).
MINB="$WORK/minbin"; mkdir -p "$MINB"
for t in env timeout sh python3 cat; do
    p=$(command -v "$t") && ln -sf "$p" "$MINB/$t"
done

# ── fixtures, all inside $WORK (deleted at the end — we keep no test media) ────
printf 'clip\n' > "$WORK/clip.mkv"
# a fleet whose card names are multi-byte (★ C1): one longer than 22 code points (must NOT be cut
# mid-character) and one shorter (must pad to 22 DISPLAY columns like Python, not 22 bytes).
"$PYTHON" - "$WORK" <<'PYEOF'
import json, sys
work = sys.argv[1]
def g(card, name, node, out=False, driver="amdgpu"):
    return {"card": card, "bdf": "0000:0%s:00.0" % card[-1], "name": name, "driver": driver,
            "vendor": "0x1002", "device": "0x0000", "integrated": False, "render_node": node,
            "vram_total_mib": 16384, "vram_used_mib": 0, "display_outputs": 1 if out else 0,
            "connectors_total": 4, "has_output": out,
            "link": {"binding_element": "0000:00:00.0", "max_width": 16, "max_gen": "Gen4",
                     "gbps_max": 54.4, "current_width": 16, "current_gen": "Gen4", "gbps_current": 54.4,
                     "idle_degraded": False, "own_reports_width": 16, "capped_by_topology": False}}
longname = "\uff32\uff38\uff19\uff10\uff17\uff10\uff38\uff34\u30c6\u30b9\u30c8\u65e5\u672c\u8a9e\u540d\u3067\u3059\u3053\u308c\u306f\u306a\u304c\u3044\u3067\u3059\u3088"
shortname = "\uff32\uff38\uff16\uff18\uff10\uff10\u65e5\u672c"
db = {"gpufan": "probe/1.0.0", "gpus": [g("card0", "RX 9070 XT", "renderD128", True),
                                      g("card1", longname, "renderD129"),
                                      g("card2", shortname, "renderD130")]}
open(work + "/fleet-nonascii.json", "w").write(json.dumps(db, indent=1))
print("      fleet-nonascii.json: long=%d cp / %d bytes, short=%d cp" %
      (len(longname), len(longname.encode()), len(shortname)))
PYEOF

# A fake HOME so `play --framegen` can never write the user's REAL ~/.config/lsfg-vk/conf.toml.
# It carries a Lossless Scaling install on the lsfg-vk branch and a conf that ALREADY names the
# profile, so ensure_lsfg_profile takes the read-only "already present" path on every build.
FAKEHOME="$WORK/fakehome"
mkdir -p "$FAKEHOME/.config/lsfg-vk" \
         "$FAKEHOME/.local/share/Steam/steamapps/common/Lossless Scaling"
: > "$FAKEHOME/.local/share/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
printf '"BetaKey" "lsfg-vk"\n' > "$FAKEHOME/.local/share/Steam/steamapps/appmanifest_993090.acf"
printf '[[profile]]\nname = "spirit 2x"\nmultiplier = 2\n' > "$FAKEHOME/.config/lsfg-vk/conf.toml"

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

# SPIRIT_DEVICES is granted to the child, never exported into this shell. CHILD_PATH / CHILD_HOME,
# when set, replace the child's PATH / HOME for a single case (special stubs, isolated config).
run_py() {
    if [ -n "$CHILD_PATH" ] && [ -n "$CHILD_HOME" ]; then
        if [ -n "$FAKE" ]; then env HOME="$CHILD_HOME" PATH="$CHILD_PATH" SPIRIT_DEVICES="$FAKE" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"
        else env HOME="$CHILD_HOME" PATH="$CHILD_PATH" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"; fi
    elif [ -n "$CHILD_PATH" ]; then
        if [ -n "$FAKE" ]; then env PATH="$CHILD_PATH" SPIRIT_DEVICES="$FAKE" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"
        else env PATH="$CHILD_PATH" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"; fi
    elif [ -n "$CHILD_HOME" ]; then
        if [ -n "$FAKE" ]; then env HOME="$CHILD_HOME" SPIRIT_DEVICES="$FAKE" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"
        else env HOME="$CHILD_HOME" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"; fi
    elif [ -n "$FAKE" ]; then env SPIRIT_DEVICES="$FAKE" timeout 60 "$PYTHON" "$PYSPIRIT" "$@"
    else timeout 60 "$PYTHON" "$PYSPIRIT" "$@"; fi
}
run_cpp() {
    if [ -n "$CHILD_PATH" ] && [ -n "$CHILD_HOME" ]; then
        if [ -n "$FAKE" ]; then env HOME="$CHILD_HOME" PATH="$CHILD_PATH" SPIRIT_DEVICES="$FAKE" timeout 60 "$CPP" "$@"
        else env HOME="$CHILD_HOME" PATH="$CHILD_PATH" timeout 60 "$CPP" "$@"; fi
    elif [ -n "$CHILD_PATH" ]; then
        if [ -n "$FAKE" ]; then env PATH="$CHILD_PATH" SPIRIT_DEVICES="$FAKE" timeout 60 "$CPP" "$@"
        else env PATH="$CHILD_PATH" timeout 60 "$CPP" "$@"; fi
    elif [ -n "$CHILD_HOME" ]; then
        if [ -n "$FAKE" ]; then env HOME="$CHILD_HOME" SPIRIT_DEVICES="$FAKE" timeout 60 "$CPP" "$@"
        else env HOME="$CHILD_HOME" timeout 60 "$CPP" "$@"; fi
    elif [ -n "$FAKE" ]; then env SPIRIT_DEVICES="$FAKE" timeout 60 "$CPP" "$@"
    else timeout 60 "$CPP" "$@"; fi
}

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

# a case run with a REPLACED child PATH (special stub set), then the shell's PATH is untouched.
cmp_cmd_path() { # path label args...
    _p=$1; label=$2; shift 2
    CHILD_PATH="$_p"
    cmp_cmd "$label" "$@"
    CHILD_PATH=""
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
    # Remove the volatile "[idle now: …]" suffix AND any trailing spaces it leaves behind: when the
    # suffix is absent the line ends with the field padding ("yes    "), when present the strip eats
    # it ("yes"), so an untrimmed compare flaps with the card's own link state.
    run_py  "$@" 2>/dev/null | sed -e 's/ *\[idle now: [^]]*\]//' -e 's/[[:space:]]*$//' > "$WORK/py.out"
    run_cpp "$@" 2>/dev/null | sed -e 's/ *\[idle now: [^]]*\]//' -e 's/[[:space:]]*$//' > "$WORK/cpp.out"
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
cmp_cmd "plan --vram-mib 1_000 (int() underscores)" plan --workload 1440p60-framegen --vram-mib 1_000
cmp_cmd "plan --vram-mib=' 12' (int() trims)"       plan --vram-mib=" 12"
cmp_cmd "plan --workload=llm-13b"                   plan --workload=llm-13b
cmp_cmd "plan unknown"          plan --workload nope
cmp_cmd "fg 1440p60"            fg --res 1440p60
cmp_cmd "fg 1440p60 round-trip" fg --res 1440p60 --mode round-trip
cmp_cmd "fg 4k120 (two-field res parse, A2)"    fg --res 4k120
cmp_cmd "fg 1080p144"           fg --res 1080p144
cmp_cmd "fg 1440p@0 (zero refresh -> 60)"       fg --res 1440p@0
cmp_cmd "fg 4k@144"             fg --res 4k@144
cmp_cmd "fg --res=4k120"        fg --res=4k120
cmp_cmd "fg bogus res"          fg --res zzz
cmp_cmd "fg no res"             fg
cmp_cmd "doctor"                doctor
cmp_cmd "boost"                 boost
cmp_cmd "no-args help"
cmp_cmd "--version (one line, exit 0)"  --version
cmp_cmd "run (encode, no cmd)"  run --role encode --card card2
# argparse help / error paths
cmp_cmd "probe -h"              probe -h
cmp_cmd "roles -h"              roles -h
cmp_cmd "plan -h"               plan -h
cmp_cmd "doctor -h"             doctor -h
cmp_cmd "boost -h"              boost -h
cmp_cmd "fg -h"                 fg -h
cmp_cmd "origin -h (--check line, C10)" origin -h
cmp_cmd "origin --bogus (C10)"  origin --bogus
cmp_cmd "origin --check"        origin --check
cmp_cmd "origin"                origin
cmp_cmd "probe --foo"           probe --foo
cmp_cmd "doctor -x"             doctor -x
cmp_cmd "boost --foo"           boost --foo
cmp_cmd "plan foo"              plan foo
cmp_cmd "plan --workload"       plan --workload
cmp_cmd "plan --vram-mib abc"   plan --vram-mib abc
cmp_cmd "fg --mode bogus"       fg --mode bogus
cmp_cmd "roles --profile=edit"  roles --profile=edit
# ★ ABBREVIATIONS MUST BE REJECTED (added 2026-10-11). argparse's allow_abbrev does NOT propagate to
# subparsers, so Python used to accept `probe --js` while both ports rejected it — a divergence created
# BY the fix for a divergence. No case could exist until the reference was repaired; now one can.
cmp_cmd "probe --js (abbrev rejected)"  probe --js
cmp_cmd "roles --profil (abbrev rejected)" roles --profil edit
cmp_cmd "fg --re (abbrev rejected)"      fg --re 4k120
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

# ★ A fixture that cannot be read is REFUSED, never silently the real machine (spec B). The
#   parse-error text is language-specific, so only the not-found case is compared byte-for-byte.
cmp_dev() { # label dev args...
    label=$1; dev=$2; shift 2
    env SPIRIT_DEVICES="$dev" timeout 60 "$PYTHON" "$PYSPIRIT" "$@" > "$WORK/py.out" 2>"$WORK/py.err"; pyrc=$?
    env SPIRIT_DEVICES="$dev" timeout 60 "$CPP" "$@" > "$WORK/cpp.out" 2>"$WORK/cpp.err"; crc=$?
    if [ "$pyrc" = "$crc" ] && diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null 2>&1 \
       && diff -q "$WORK/py.err" "$WORK/cpp.err" >/dev/null 2>&1; then
        echo "PASS  $label"; pass=$((pass+1))
    else echo "FAIL  $label (py=$pyrc cpp=$crc)"; fail=$((fail+1));
         diff "$WORK/py.err" "$WORK/cpp.err" | head -10; fi
}
cmp_dev "probe: SPIRIT_DEVICES missing -> refuse, rc2" "$WORK/does-not-exist.json" probe
printf '{ not json' > "$WORK/bad.json"
env SPIRIT_DEVICES="$WORK/bad.json" "$PYTHON" "$PYSPIRIT" probe >/dev/null 2>&1; pyrc=$?
env SPIRIT_DEVICES="$WORK/bad.json" "$CPP" probe >/dev/null 2>&1; crc=$?
if [ "$pyrc" = "$crc" ]; then echo "PASS  probe: SPIRIT_DEVICES malformed -> rc $crc"; pass=$((pass+1))
else echo "FAIL  probe: malformed (py=$pyrc cpp=$crc)"; fail=$((fail+1)); fi

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
    if diff -q "$WORK/py.gs" "$WORK/cpp.gs" >/dev/null 2>&1; then echo "PASS  $lbl gamescope-argv"; pass=$((pass+1))
    else echo "FAIL  $lbl gamescope-argv"; fail=$((fail+1)); diff "$WORK/py.gs" "$WORK/cpp.gs"|head -30; fi
    pkill -f "$STUB/unshare" 2>/dev/null; pkill -f "$STUB/ffmpeg" 2>/dev/null; pkill -f "$STUB/gamescope" 2>/dev/null
done

echo "--- play: --work / --split / --work-outdir / --spare / --overlay (★ new coverage) ---"
cmp_cmd "play --no-offload --work (A1: no job, no-offload reason)" \
        play --no-offload --work "$WORK/clip.mkv" -- /bin/echo HI
cmp_cmd "play --work missing file (uniform REFUSED)" \
        play --work "$WORK/no-such-input.mkv" -- /bin/echo HI
cmp_cmd "play --spare unknown node (C6 notice + REFUSED list)" \
        play --spare renderD999 -- /bin/echo HI
cmp_cmd "play --spare repeated node (C6 dedupe)" \
        play --spare renderD129,renderD129 -- /bin/echo HI
cmp_cmd_path "$MINB" "play --overlay, gamescope absent (A5 refusal before spawn)" \
        play --overlay -- /bin/echo HI
cmp_cmd_path "$MINB" "play missing game command (B: rc127 + clean message)" \
        play --no-offload -- /no/such/spirit-game
cmp_cmd "play signalled game (C2: -signum)" play --no-offload -- sh -c 'kill -TERM $$'
cmp_cmd "play Wine/.exe target"   play --no-offload -- /tmp/spirit-nogame.exe
# job cases need the fast ffmpeg so a transcode is not a 30 s wait
cmp_cmd_path "$FASTB" "play --work (replication JOB, C9 basename)" \
        play --work "$WORK/clip.mkv" -- /bin/echo HI
cmp_cmd_path "$FASTB" "play --work-outdir x/../b (C7 abspath normalisation)" \
        play --work "$WORK/clip.mkv" --work-outdir "$WORK/x/../b" -- /bin/echo HI
cmp_cmd_path "$FASTB" "play --split (SPLIT JOB across cards)" \
        play --split --work "$WORK/clip.mkv" -- /bin/echo HI
# split with an unreadable duration -> split: REFUSED, BASENAME (C8). No ffprobe stub here, so the
# live ffprobe fails on a text file and _media_duration returns None.
NOPROBE="$WORK/noprobe"; mkdir -p "$NOPROBE"; cp "$FASTB/unshare" "$FASTB/gamescope" "$FASTB/ffmpeg" "$NOPROBE/"
cmp_cmd_path "$NOPROBE" "play --split unreadable duration (C8 basename)" \
        play --split --work "$WORK/clip.mkv" -- /bin/echo HI
# a test load that dies before the game does (B: poll the loads, do not assume they ran)
cmp_cmd_path "$FASTB" "play test load exits early (B: EXITED EARLY)" \
        play --offload -- sh -c 'sleep 1'
# non-ASCII card name (C1): code-point slicing + display-width padding
FAKE="$WORK/fleet-nonascii.json"
cmp_cmd "roles non-ASCII card name (C1)"  roles
cmp_cmd "probe non-ASCII card name (C1)"  probe
cmp_cmd "play non-ASCII card name tail (C1)" play --offload -- /bin/echo HI
FAKE="$FLEET"

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
# ★ ISOLATE THE lsfg-vk CONFIG. `play --framegen` reaches _ensure_lsfg_profile, which APPENDS to
#   ~/.config/lsfg-vk/conf.toml and copies it aside. A test run must never touch the user's own
#   file, so the child gets a fake HOME whose conf already names the profile (read-only path).
CHILD_HOME="$FAKEHOME"
cmp_cmd "play --framegen 2 (real, isolated HOME)" play --framegen 2 --no-offload -- /bin/echo HI
CHILD_HOME=""
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
