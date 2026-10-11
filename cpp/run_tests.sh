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
# ★ ASSERT THE FIXTURES EXIST (spec B2). If fleet-10.example.json is missing, every fake-fleet case
# silently becomes "both sides refuse identically" and PASSES — a suite that tests nothing while looking
# green. Fail loudly here instead.
[ -f "$FLEET" ]   || { echo "FIXTURE MISSING: $FLEET (fake-fleet cases would silently pass)" >&2; exit 1; }

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
# ★ RECORD ARGV AND NEVER TOUCH /dev/dri (spec B1). The real `unshare -rm` runs the hide script, which
# bind-mounts /dev/null over each of the OTHER cards' render nodes — harmless as a user, but as ROOT it
# would bind over the host's LIVE GPU nodes. So this stub never runs the script. For the mount form
# (`-rm sh -c SCRIPT sh HIDELIST cmd...`) it prints the same stderr line the real (unprivileged) path
# prints for each node it cannot hide, then runs the wrapped command, so the game's own output and exit
# code are still exercised. For any other form (e.g. doctor's `unshare -Urm true`) it just runs the tail.
{ for a in "$@"; do printf 'ARG[%s]\n' "$a"; done; } >> "$STUB_UN"
if [ "${1:-}" = "-rm" ] && [ "${3:-}" = "-c" ]; then
    shift 4                 # drop: -rm sh -c SCRIPT
    shift                   # drop the inner `sh` (its $0)
    hide="$1"; shift        # the space-joined hide list, then the wrapped command
    for n in $hide; do
        echo "  wall: could NOT hide /dev/dri/$n - the game can still see this card" >&2
    done
    exec "$@"
fi
shift                       # drop the leading flag (e.g. -Urm) and run the rest
exec "$@"
EOF
cat > "$STUB/ffmpeg" <<'EOF'
#!/bin/sh
printf 'FFMPEG ARGS: %s\nLIBVA=%s\n' "$*" "$LIBVA_DRIVER_NAME" >> "$STUB_FF"
exec sleep 30
EOF
cat > "$STUB/ffprobe" <<'EOF'
#!/bin/sh
# ★ REPORT A REAL DURATION FOR A REAL FILE (spec B3). For a file whose content is a plain number, echo
# that number; otherwise fall back to 60.000. This lets the split stub's slices sum back to the
# original, exercising the COMPLETE/INCOMPLETE arithmetic rather than forcing every case to MISSING.
f=""
for a in "$@"; do f="$a"; done
if [ -f "$f" ]; then
    d=$(cat "$f" 2>/dev/null)
    case "$d" in
        ''|*[!0-9.]*) echo "60.000" ;;
        *) echo "$d" ;;
    esac
else
    echo "60.000"
fi
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
# steam: exits 0 at once, standing in for a running Steam client that accepts a hand-off and returns.
# (`steam://...` is NOT stubbed — a real `steam://` URL is not a program, so both builds must report
#  the exec failure identically; that is the OTHER steam case below.)
cat > "$STUB/steam" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$STUB/unshare" "$STUB/ffmpeg" "$STUB/ffprobe" "$STUB/vulkaninfo" "$STUB/gamescope" "$STUB/steam"

# ── auxiliary stub sets (★ new coverage) ──────────────────────────────────────
# FASTB: same stubs but ffmpeg writes a REAL output file and exits at once, so a --work job finishes the
#        moment the game does (the default ffmpeg sleeps 30 s, which would make every job case a wait).
#        ★ It writes the requested slice duration (its -t value, default 60) into the output file, so the
#        on-disk check, the JOB OUTPUT line and the SPLIT CHECK arithmetic are actually exercised
#        (spec B3) — a split's slices then sum back to the original instead of every output being MISSING.
FASTB="$WORK/fastbin"; mkdir -p "$FASTB"
cp "$STUB/unshare" "$STUB/ffprobe" "$STUB/vulkaninfo" "$STUB/gamescope" "$FASTB/"
cat > "$FASTB/ffmpeg" <<'EOF'
#!/bin/sh
dur=60; prev=""
for a in "$@"; do
    [ "$prev" = "-t" ] && dur="$a"
    prev="$a"
done
out=""
for a in "$@"; do out="$a"; done
# ★ DO NOT WRITE A FILE NAMED `-` (harness hygiene). For a test load the last argv is `-` (ffmpeg's
# `-f null -`), so the old stub dropped a bare file named `-` in the CURRENT DIRECTORY — which
# contradicts "nothing is written outside $TMPDIR". Only write when the target is a real path.
[ -n "$out" ] && [ "$out" != "-" ] && printf '%s\n' "$dur" > "$out" 2>/dev/null
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
# EARLY: the fast (output-writing) stubs PLUS a real sh/sleep, used to observe a test load that runs to
# completion before the game does (a clean rc 0 -> "COMPLETED its Ns bounded run, as designed").
EARLY="$WORK/earlybin"; mkdir -p "$EARLY"
cp "$FASTB/unshare" "$FASTB/gamescope" "$FASTB/ffmpeg" "$FASTB/ffprobe" "$EARLY/"
for t in sh sleep env cat; do
    p=$(command -v "$t") && ln -sf "$p" "$EARLY/$t"
done
# DEADB: fast stubs but ffmpeg exits NONZERO at once — observes a test load that DIED (rc != 0) before
# its bounded run finished. EARLY's ffmpeg exits 0, so it can only ever exercise the COMPLETED arm.
DEADB="$WORK/deadbin"; mkdir -p "$DEADB"
cp "$FASTB/unshare" "$FASTB/gamescope" "$FASTB/ffprobe" "$DEADB/"
cat > "$DEADB/ffmpeg" <<'EOF'
#!/bin/sh
exit 1
EOF
for t in sh sleep env cat; do
    p=$(command -v "$t") && ln -sf "$p" "$DEADB/$t"
done
chmod +x "$DEADB"/*
# BADSUB: like FASTB but every output file claims a 5 s duration, so a split's slices do NOT add back up
# to the 60 s original — this is what exercises the INCOMPLETE arm of the split arithmetic.
BADSUB="$WORK/badsub"; mkdir -p "$BADSUB"
cp "$STUB/unshare" "$STUB/ffprobe" "$STUB/gamescope" "$BADSUB/"
cat > "$BADSUB/ffmpeg" <<'EOF'
#!/bin/sh
out=""; for a in "$@"; do out="$a"; done
[ -n "$out" ] && [ "$out" != "-" ] && printf '5\n' > "$out" 2>/dev/null
exit 0
EOF
chmod +x "$BADSUB"/*

# ★ LAUNCHER TOOLS FOR THE SPECIAL PATH SETS (fix 2026-10-11). cmp_cmd_path REPLACES the child's PATH
# wholesale, but every case that uses one is launched as `env PATH=$SPECIAL timeout 60 python3|./spirit …`.
# FASTB/BADSUB/NOPROBE/EARLY/DEADB were built from the STUB set alone, so they never contained `timeout`
# or `python3`: the child died at exec with `env: 'timeout': No such file or directory` (rc 127) on BOTH
# sides, before spirit was ever reached. Every case using them therefore passed on identical error text
# and could not fail — a suite that tested nothing while looking green. Give them the launcher, and `sh`
# (the unshare hide-wrapper runs `sh -c`), while leaving each set's DELIBERATE omissions intact
# (NOPROBE's ffprobe, MINB's unshare/gamescope/vulkaninfo). MINB already carries the launcher.
for d in "$FASTB" "$BADSUB" "$EARLY" "$DEADB"; do
    for t in timeout python3 sh; do
        [ -e "$d/$t" ] || { p=$(command -v "$t") && ln -sf "$p" "$d/$t"; }
    done
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

# ★ TWO MORE DETERMINISTIC FIXTURES for the A4 marginal rule and the doctor FOUR-VERDICT redesign.
def card(cname, node, w, capped=False, out=False, vendor="0x1002", device="0x6860"):
    return {"card": cname, "bdf": "0000:0%s:00.0" % cname[-1], "name": "Test GPU " + cname,
            "driver": "amdgpu", "vendor": vendor, "device": device, "integrated": False,
            "render_node": node, "vram_total_mib": 16384, "vram_used_mib": 0,
            "display_outputs": 1 if out else 0, "connectors_total": 4, "has_output": out,
            "link": {"binding_element": "0000:00:00.0", "max_width": w, "max_gen": "Gen3",
                     "gbps_max": 8.0, "current_width": w, "current_gen": "Gen3", "gbps_current": 8.0,
                     "idle_degraded": False, "own_reports_width": 16, "capped_by_topology": capped}}
# fleet-marginal.json: main card0 (x16, has a panel); spare card1 on a capped x4 link — MARGINAL by the
# capped_by_topology rule, so a default `--work` must NOT use it and must NAME it before launch.
mdb = {"gpufan": "probe/1.0.0", "gpus": [card("card0", "renderD128", 16, out=True),
                                          card("card1", "renderD129", 4, capped=True)]}
open(work + "/fleet-marginal.json", "w").write(json.dumps(mdb, indent=1))
# fleet-dupe.json: two cards sharing one vendor:device id — doctor's individual-pin check must FAIL.
ddb = {"gpufan": "probe/1.0.0", "gpus": [card("card0", "renderD128", 16, out=True),
                                          card("card1", "renderD129", 16)]}
open(work + "/fleet-dupe.json", "w").write(json.dumps(ddb, indent=1))
# fleet-jobs.json: a main card plus TWO clean x16 amdgpu spares (distinct ids, not marginal), so a
# --work job actually runs and a --split divides the input across two cards. fleet-10's spares are all
# on marginal links, which the default now (correctly) declines — so the job path needs this fixture.
import copy
j0 = card("card0", "renderD128", 16, out=True, device="0x6860")
j1 = card("card1", "renderD129", 16, device="0x731f")
j2 = card("card2", "renderD130", 16, device="0x67df")
jdb = {"gpufan": "probe/1.0.0", "gpus": [j0, j1, j2]}
open(work + "/fleet-jobs.json", "w").write(json.dumps(jdb, indent=1))
print("      fleet-marginal.json, fleet-dupe.json, fleet-jobs.json written")
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
cmp_cmd "play --test-load=abc (exit 2)" play --test-load=abc -- /bin/echo HI
cmp_cmd "play --test-load (no value -> 60, no cmd)" play --test-load
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
for o in "default" "--no-offload"; do
    arg=""; [ "$o" = "--no-offload" ] && arg="--no-offload"
    : > "$STUB_UN"; : > "$STUB_FF"
    run_py  play $arg -- /bin/echo HELLO > "$WORK/py.out" 2>&1
    cp "$STUB_UN" "$WORK/py.un"; cp "$STUB_FF" "$WORK/py.ff"
    : > "$STUB_UN"; : > "$STUB_FF"
    run_cpp play $arg -- /bin/echo HELLO > "$WORK/cpp.out" 2>&1
    cp "$STUB_UN" "$WORK/cpp.un"; cp "$STUB_FF" "$WORK/cpp.ff"
    lbl="play [$o] -- /bin/echo"
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then echo "PASS  $lbl stdout"; pass=$((pass+1))
    else echo "FAIL  $lbl stdout"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out"|head -20; fi
    if diff -q "$WORK/py.un" "$WORK/cpp.un" >/dev/null 2>&1; then echo "PASS  $lbl unshare-argv"; pass=$((pass+1))
    else echo "FAIL  $lbl unshare-argv"; fail=$((fail+1)); diff "$WORK/py.un" "$WORK/cpp.un"|head -30; fi
    # ★ THE DEFAULT NOW STARTS NOTHING (spec A4/B3). The old default put a synthetic encode on every
    # spare; the case used to assert that encode's argv and so "could not fail" once the default stopped
    # doing it. It now asserts the OPPOSITE — that ffmpeg was NOT invoked at all.
    if [ "$o" = "default" ]; then
        if [ ! -s "$WORK/py.ff" ] && [ ! -s "$WORK/cpp.ff" ]; then
            echo "PASS  $lbl starts no encode (default = no work)"; pass=$((pass+1))
        else echo "FAIL  $lbl default started an encode"; fail=$((fail+1)); cat "$WORK/cpp.ff"; fi
        # the tail must actually SAY IDLE — an empty tail would pass the diff above but read as broken.
        if grep -q 'IDLE  (no job given)' "$WORK/cpp.out"; then
            echo "PASS  $lbl tail prints IDLE"; pass=$((pass+1))
        else echo "FAIL  $lbl tail missing IDLE"; fail=$((fail+1)); sed -n '1,40p' "$WORK/cpp.out"; fi
    fi
    pkill -f "$STUB/unshare" 2>/dev/null; pkill -f "$STUB/ffmpeg" 2>/dev/null; pkill -f "$STUB/gamescope" 2>/dev/null
done

# ★ --test-load SECS (A4): a paced, bounded encode on the spares. Assert the exact ffmpeg argv carries
# -re and -t SECS and the stdout carries the TEST LOAD block, so the opt-in bounded load is really tested.
echo "--- play --test-load: opt-in bounded load ---"
# ★ fleet-10's spares are almost all on marginal links (so the default correctly declines them); use a
# fixture whose spare is a clean x16 amdgpu, so a test load is actually STARTED and its argv observable.
FAKE="$WORK/fleet-dupe.json"
: > "$STUB_UN"; : > "$STUB_FF"
run_py  play --test-load 30 -- sh -c 'sleep 1' > "$WORK/py.out" 2>&1
cp "$STUB_FF" "$WORK/py.ff"
: > "$STUB_FF"
run_cpp play --test-load 30 -- sh -c 'sleep 1' > "$WORK/cpp.out" 2>&1
cp "$STUB_FF" "$WORK/cpp.ff"
sort "$WORK/py.ff" > "$WORK/py.ffs"; sort "$WORK/cpp.ff" > "$WORK/cpp.ffs"
if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null && diff -q "$WORK/py.ffs" "$WORK/cpp.ffs" >/dev/null \
   && [ -s "$WORK/cpp.ffs" ] && grep -q -- '-t 30' "$WORK/cpp.ff" && grep -q -- '-re' "$WORK/cpp.ff"; then
    echo "PASS  play --test-load 30 (bounded load argv; still running at game exit)"; pass=$((pass+1))
else echo "FAIL  play --test-load 30 (py=$pyrc cpp=$crc)"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out"|head -20; cat "$WORK/cpp.ff"; fi
pkill -f "$STUB/unshare" 2>/dev/null; pkill -f "$STUB/ffmpeg" 2>/dev/null; pkill -f "$STUB/gamescope" 2>/dev/null
FAKE="$FLEET"

echo "--- play: the Steam path (blocking item 2 — said, not silent) ---"
# ★ A STEAM LAUNCH CANNOT BE PINNED FROM HERE, AND BOTH BUILDS MUST SAY SO. Two claims, two cases:
#   (1) `steam://rungameid/N` is not a program: both must print the launch-options RECIPE and then the
#       same exec-failure line (rc 127).
#   (2) a launcher that RETURNS AT ONCE (`steam`, stubbed to exit 0) must be named as having routed
#       NOTHING — the old code printed `game exited (rc=0)` as if the session had worked.
# The recipe line carries each build's OWN absolute path, and the fast-return line carries a measured
# time; normalize both, because the CLAIM is the message, not the path or the clock.
_steam_norm() {
    sed -e 's#".*" play -- %command%#"<SELF>" play -- %command%#' \
        -e 's/after [0-9][0-9.]*s/after N.Ns/'
}
cmp_steam() { # label args...
    label=$1; shift
    run_py  "$@" > "$WORK/py.raw" 2>/dev/null; pyrc=$?
    run_cpp "$@" > "$WORK/cpp.raw" 2>/dev/null; crc=$?
    _steam_norm < "$WORK/py.raw" > "$WORK/py.out"
    _steam_norm < "$WORK/cpp.raw" > "$WORK/cpp.out"
    if [ "$pyrc" != "$crc" ]; then
        echo "FAIL  $label   (exit py=$pyrc cpp=$crc)"; fail=$((fail+1)); return
    fi
    if diff -q "$WORK/py.out" "$WORK/cpp.out" >/dev/null; then
        echo "PASS  $label"; pass=$((pass+1))
    else echo "FAIL  $label"; fail=$((fail+1)); diff "$WORK/py.out" "$WORK/cpp.out" | head -20; fi
}
cmp_steam "play -- steam:// (says it cannot be pinned; prints the recipe)" \
        play --no-offload -- steam://rungameid/123
cmp_steam "play -- steam (fast clean exit -> session did NOT route the game)" \
        play --no-offload -- steam
# the recipe and the not-routed line must ACTUALLY be on the page, not merely identical between builds.
if grep -q 'play -- %command%' "$WORK/cpp.out" && grep -q 'did NOT route the game' "$WORK/cpp.out"; then
    echo "PASS  steam message present (recipe + not-routed)"; pass=$((pass+1))
else echo "FAIL  steam message missing from the output"; fail=$((fail+1)); cat "$WORK/cpp.out"; fi

echo "--- play --scale / --framegen: stdout + exact gamescope argv (gamescope stubbed) ---"
# ★ ISOLATE HOME FOR EVERY --framegen CASE (spec B4): _ensure_lsfg_profile appends to
#   ~/.config/lsfg-vk/conf.toml and copies it aside, so a test run must never touch the user's own file.
CHILD_HOME="$FAKEHOME"
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
CHILD_HOME=""

echo "--- play: --work / --split / --work-outdir / --spare / --overlay (★ new coverage) ---"
# ★ fleet-10's spares are all on marginal links now, which the default job path declines; the fixtures
# below use clean x16 spares so the JOB path, the on-disk output check and the SPLIT arithmetic run.
FAKE="$WORK/fleet-jobs.json"
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
# ★ the INCOMPLETE arm: slices that do not add back up. BADSUB's ffmpeg writes a 5 s duration per slice,
# so 2 slices sum to 10 s against a 60 s original — the split check must say INCOMPLETE, not ok.
cmp_cmd_path "$BADSUB" "play --split INCOMPLETE (B3: arithmetic really checked)" \
        play --split --work "$WORK/clip.mkv" -- /bin/echo HI
# split with an unreadable duration -> split: REFUSED, BASENAME (C8). No ffprobe stub here, so the
# live ffprobe fails on a text file and _media_duration returns None.
NOPROBE="$WORK/noprobe"; mkdir -p "$NOPROBE"; cp "$FASTB/unshare" "$FASTB/gamescope" "$FASTB/ffmpeg" "$NOPROBE/"
# ★ the same launcher fix as the other special sets (ffprobe stays absent on purpose, so the duration
# read fails and the split refusal path is genuinely exercised).
for t in timeout python3 sh; do p=$(command -v "$t") && ln -sf "$p" "$NOPROBE/$t"; done
cmp_cmd_path "$NOPROBE" "play --split unreadable duration (C8 basename)" \
        play --split --work "$WORK/clip.mkv" -- /bin/echo HI
FAKE="$FLEET"
# a test load that runs to completion before the game does (B: COMPLETED), and one that DIES (B: DIED).
# ★ The old case used `--offload` (not a real flag, so it fell through and never started a load) — it
# could not fail as labelled. EARLY's ffmpeg exits 0 at once, so the load COMPLETES; DEADB's exits 1
# at once, so the load DIED. Both use a game that outlives the load.
FAKE="$WORK/fleet-jobs.json"
cmp_cmd_path "$EARLY" "play --test-load completes (B: COMPLETED its bounded run)" \
        play --test-load 1 -- sh -c 'sleep 1'
cmp_cmd_path "$DEADB" "play --test-load dies (B: DIED, rc != 0)" \
        play --test-load 1 -- sh -c 'sleep 1'
FAKE="$FLEET"
# ★ A1: `--version` and `--json` AFTER the `--` belong to the GAME, not to us.
cmp_cmd "play -- ./game --version (A1: launches, prints no version line)" \
        play -- /bin/echo --version
cmp_cmd "play -- ./game --json (A1: the mark IS still printed)" \
        play -- /bin/echo --json
# non-ASCII card name (C1): code-point slicing + display-width padding
FAKE="$WORK/fleet-nonascii.json"
cmp_cmd "roles non-ASCII card name (C1)"  roles
cmp_cmd "probe non-ASCII card name (C1)"  probe
cmp_cmd "play non-ASCII card name tail (C1)" play -- /bin/echo HI
FAKE="$FLEET"

# ★ A4 MARGINAL RULE: a spare on a marginal link is NOT used by default and is NAMED before launch;
# --spare naming it includes it. fleet-marginal.json's card1 is a capped x4 link.
FAKE="$WORK/fleet-marginal.json"
cmp_cmd_path "$FASTB" "play --work marginal card excluded (A4: NOT using + Add --spare)" \
        play --work "$WORK/clip.mkv" -- /bin/echo HI
cmp_cmd_path "$FASTB" "play --work --spare renderD129 (A4: marginal override)" \
        play --spare renderD129 --work "$WORK/clip.mkv" -- /bin/echo HI
# ★ A5 doctor: duplicate vendor:device ids -> individual-pin FAIL -> PROBLEMS FOUND (exit 1)
FAKE="$WORK/fleet-dupe.json"
cmp_cmd "doctor duplicate vid:did (A5: exit 1, individual-pin FAIL)" doctor
# ★ A5: assert the THREE overall outcomes by their EXIT CODES EXPLICITLY (not merely that the port
# agrees with Python), for the deterministic states reachable here.
#   exit 1 — any FAIL (duplicate cards).
run_cpp doctor >/dev/null 2>&1; crc=$?
if [ "$crc" = "1" ]; then echo "PASS  doctor exit code 1 (any FAIL)"; pass=$((pass+1));
else echo "FAIL  doctor exit code 1 (got $crc)"; fail=$((fail+1)); fi
#   exit 3 — no FAIL but a CORE UNKNOWN (vulkaninfo absent -> the device pin is untestable).
FAKE=""
CHILD_PATH="$MINB"
run_cpp doctor >/dev/null 2>&1; crc=$?
if [ "$crc" = "3" ]; then echo "PASS  doctor exit code 3 (not fully verified)"; pass=$((pass+1));
else echo "FAIL  doctor exit code 3 (got $crc)"; fail=$((fail+1)); fi
CHILD_PATH=""
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
# ★ A5: doctor in the TOOLS-ABSENT state — PATH without vulkaninfo/unshare/gamescope/lspci. It must
# report NOT VERIFIED (exit 3) with NO FAIL lines, not a fabricated green.
cmp_cmd_path "$MINB" "doctor tools-absent (A5: exit 3, no FAIL lines)" doctor
#   exit 0 — ALL CHECKS PASSED (the live box has a working pin and names a display card).
run_cpp doctor >/dev/null 2>&1; crc=$?
if [ "$crc" = "0" ]; then echo "PASS  doctor exit code 0 (all checks passed)"; pass=$((pass+1));
else echo "FAIL  doctor exit code 0 (got $crc)"; fail=$((fail+1)); fi
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
rm -f "$HERE/-"          # a stray `-` cannot be left behind even if a stub ever writes one
echo
echo "================  $pass passed, $fail failed  ================"
[ "$fail" = 0 ]
