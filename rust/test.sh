#!/bin/sh
# parity test — diff the Rust `spirit` against the Python original on identical inputs.
# Usage:  ./test.sh            (uses the parent spirit.py + example fleets)
#         PY=/path/to/spirit.py ./test.sh
#
# It builds nothing and modifies nothing; it only reads the Python and runs both CLIs.
# SPIRIT_DEVICES is set PER CHILD COMMAND (never exported), so it cannot leak into the
# caller's shell and poison later readings with a fake fleet.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PARENT=$(cd "$HERE/.." && pwd)
PY=${PY:-$PARENT/spirit.py}
RS=${RS:-$HERE/target/release/spirit}
FLEET10=$PARENT/fleet-10.example.json
FLEETWEAK=$PARENT/fleet-monitor-on-weak.example.json

[ -x "$RS" ] || { echo "build first: cargo build --release" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 required" >&2; exit 1; }

pass=0; fail=0
compare() {                        # compare <label> <SPIRIT_DEVICES-or-empty> <args...>
  label=$1; dev=$2; shift 2
  if [ -n "$dev" ]; then
    SPIRIT_DEVICES="$dev" python3 "$PY" "$@" >"$TMP/py.out" 2>"$TMP/py.err"; prc=$?
    SPIRIT_DEVICES="$dev" "$RS"      "$@" >"$TMP/rs.out" 2>"$TMP/rs.err"; rrc=$?
  else
    python3 "$PY" "$@" >"$TMP/py.out" 2>"$TMP/py.err"; prc=$?
    "$RS"      "$@" >"$TMP/rs.out" 2>"$TMP/rs.err"; rrc=$?
  fi
  if [ "$prc" != "$rrc" ] || ! diff -q "$TMP/py.out" "$TMP/rs.out" >/dev/null \
                          || ! diff -q "$TMP/py.err" "$TMP/rs.err" >/dev/null; then
    echo "FAIL: $label   (py rc=$prc rs rc=$rrc)"
    diff -u "$TMP/py.out" "$TMP/rs.out" | head -40
    diff -u "$TMP/py.err" "$TMP/rs.err" | head -40
    fail=$((fail+1))
  else
    echo "PASS: $label   (rc=$prc)"
    pass=$((pass+1))
  fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== fake-device parity (deterministic) =="
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
echo "== boost (v0.2: reads the live gamescope / lsfg-vk / sysfs state; same for both sides) =="
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

echo
echo "== play --scale / --framegen argument surface (v0.2; exit-2 cases only) =="
compare "play --scale 1.5"              "$FLEET10" play --scale 1.5 --no-offload -- /bin/echo HI
compare "play --scale abc"             "$FLEET10" play --scale abc --no-offload -- /bin/echo HI
compare "play --scale (no value)"      "$FLEET10" play --scale
compare "play --framegen 7"            "$FLEET10" play --framegen 7 --no-offload -- /bin/echo HI
compare "play --framegen abc"          "$FLEET10" play --framegen abc --no-offload -- /bin/echo HI
# positive --framegen on this box resolves to an UNAVAILABLE lsfg state, so it launches
# without writing any config (state-free, deterministic). A box with a working install
# would append a profile instead — that path is exercised by the real-box diff script.
compare "play --framegen 2"            "$FLEET10" play --framegen 2 --no-offload -- /bin/echo HI

echo
echo "== doctor (fake fleet; uses real vulkaninfo/gamescope/lsfg, identical on both sides) =="
compare "doctor (fleet-10)"             "$FLEET10"   doctor

echo
echo "== help / usage surface =="
compare "no args"                       ""          
compare "empty subcommand arg"          ""          ""
compare "top-level --help"              ""          --help
for c in probe roles plan doctor fg boost; do compare "$c -h" "" "$c" -h; done
compare "unknown subcommand"            ""          frobnicate
compare "unrecognized arg (probe)"      ""          probe --bogus
compare "unrecognized trailing"         ""          probe --json extra
compare "unrecognized arg (boost)"      ""          boost --bogus
compare "missing value (roles)"         ""          roles --profile
compare "bad --mode"                    ""          fg --mode sideways
compare "bad --vram-mib"                ""          plan --vram-mib notanint

echo
echo "== summary: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
