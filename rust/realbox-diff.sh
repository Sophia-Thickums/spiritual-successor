#!/bin/sh
# real-box byte-diff: Rust spirit vs python3 ../spirit.py, SPIRIT_DEVICES unset.
# The framegen case writes ~/.config/lsfg-vk/conf.toml, so it is run with the conf
# reset to a pristine snapshot before EACH side, and the pristine conf is restored at exit.
set -u
SRC="~/Desktop/Sophia Life/WORLD/work/MULTIGPU-GAMING/spiritual-successor"
PY="$SRC/spirit.py"
RS="$SRC/rust/target/release/spirit"
CONF="$HOME/.config/lsfg-vk/conf.toml"
BAK="$CONF.testbak"
OUT=$(mktemp -d)

cp "$CONF" "$BAK" || exit 1
if grep -q 'spirit [0-9]x' "$BAK"; then echo "ABORT: pristine conf already has a spirit profile"; exit 1; fi
trap 'cp "$BAK" "$CONF"; rm -f "$BAK" "$CONF.bak-spirit"; rm -rf "$OUT"' EXIT

pass=0; fail=0
report() { # report <label> <py_rc> <rs_rc>
  label=$1; prc=$2; rrc=$3
  if [ "$prc" != "$rrc" ] || ! diff -q "$OUT/py.out" "$OUT/rs.out" >/dev/null \
                          || ! diff -q "$OUT/py.err" "$OUT/rs.err" >/dev/null; then
    echo "FAIL: $label   (py rc=$prc rs rc=$rrc)"
    echo "  --- stdout diff ---"; diff -u "$OUT/py.out" "$OUT/rs.out" | head -60
    echo "  --- stderr diff ---"; diff -u "$OUT/py.err" "$OUT/rs.err" | head -60
    fail=$((fail+1))
  else
    echo "PASS: $label   (rc=$prc)"; pass=$((pass+1))
  fi
}

compare() {           # stateless
  label=$1; shift
  env -u SPIRIT_DEVICES python3 "$PY" "$@" >"$OUT/py.out" 2>"$OUT/py.err"; prc=$?
  env -u SPIRIT_DEVICES "$RS"      "$@" >"$OUT/rs.out" 2>"$OUT/rs.err"; rrc=$?
  report "$label" "$prc" "$rrc"
}

compare_fg() {        # stateful: reset conf before each side
  label=$1; shift
  cp "$BAK" "$CONF"; rm -f "$CONF.bak-spirit"
  env -u SPIRIT_DEVICES python3 "$PY" "$@" >"$OUT/py.out" 2>"$OUT/py.err"; prc=$?
  cp "$BAK" "$CONF"; rm -f "$CONF.bak-spirit"
  env -u SPIRIT_DEVICES "$RS"      "$@" >"$OUT/rs.out" 2>"$OUT/rs.err"; rrc=$?
  report "$label" "$prc" "$rrc"
}

echo "== boost / doctor / help =="
compare "boost"            boost
compare "doctor"           doctor
compare "--help"           --help
compare "-h"               -h
compare "no args"
compare "boost -h"         boost -h

echo
echo "== play --scale (real gamescope; state-free) =="
compare "play --scale 0.7" play --scale 0.7 --no-offload -- /bin/echo HI
compare "play --scale 1.0" play --scale 1.0 --no-offload -- /bin/echo HI
compare "play --scale 0.3" play --scale 0.3 --no-offload -- /bin/echo HI

echo
echo "== play --scale / --framegen errors (exit 2) =="
compare "play --scale 1.5"        play --scale 1.5 --no-offload -- /bin/echo HI
compare "play --scale abc"        play --scale abc --no-offload -- /bin/echo HI
compare "play --scale (no value)" play --scale
compare "play --framegen 7"       play --framegen 7 --no-offload -- /bin/echo HI
compare "play --framegen abc"     play --framegen abc --no-offload -- /bin/echo HI

echo
echo "== play --framegen (stateful conf; reset before each side) =="
compare_fg "play --framegen 2"    play --framegen 2 --no-offload -- /bin/echo HI
compare_fg "play --framegen 4"    play --framegen 4 --no-offload -- /bin/echo HI

echo
echo "== summary: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
