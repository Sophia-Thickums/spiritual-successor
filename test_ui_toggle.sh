#!/bin/bash
# GUI ON/OFF TOGGLE — artifact-level test, with a FAKE HOME and a STUBBED systemctl so the REAL
# session is never armed. Verifies what actually happened (the manager env + the file), not the
# response body.
SS="$(cd "$(dirname "$0")" && pwd)"
FH=/tmp/guitest
rm -rf "$FH"; mkdir -p "$FH/.config" "$FH/bin"
: > "$FH/manager.env"

cat > "$FH/bin/systemctl" <<'EOS'
#!/bin/sh
# Minimal systemd-user-manager stand-in. Records every call.
echo "$@" >> "$FAKEHOME/systemctl.log"
if [ "$1" = "--user" ]; then
  case "$2" in
    show-environment) cat "$FAKEHOME/manager.env" 2>/dev/null; exit 0 ;;
    set-environment)  shift 2
        for kv in "$@"; do
          k=${kv%%=*}
          grep -v "^${k}=" "$FAKEHOME/manager.env" > "$FAKEHOME/.e" 2>/dev/null
          mv "$FAKEHOME/.e" "$FAKEHOME/manager.env" 2>/dev/null
          printf '%s\n' "$kv" >> "$FAKEHOME/manager.env"
        done
        exit 0 ;;
    unset-environment) shift 2
        for k in "$@"; do
          grep -v "^${k}=" "$FAKEHOME/manager.env" > "$FAKEHOME/.e" 2>/dev/null
          mv "$FAKEHOME/.e" "$FAKEHOME/manager.env" 2>/dev/null
        done
        exit 0 ;;
  esac
fi
exit 0
EOS
chmod +x "$FH/bin/systemctl"

cd "$SS"
# The REAL session, captured before anything runs. HOME is faked below, so ask the manager directly.
env systemctl --user show-environment > "$FH/real-before" 2>/dev/null || true
export HOME="$FH" FAKEHOME="$FH"
export PATH="$FH/bin:$PATH"

cd "$SS"
python3 spirit_ui.py --print-url --no-open --port 8899 > "$FH/ui.log" 2>&1 &
UIPID=$!
for i in $(seq 1 40); do sleep 0.5; grep -q "http" "$FH/ui.log" && break; done
URL=$(grep -o 'http://127.0.0.1:[0-9]*/[^ ]*/' "$FH/ui.log" | head -1)
echo "URL: $URL"
PASS=0; FAIL=0
chk(){ if [ "$2" = "$3" ]; then echo "PASS  $1"; PASS=$((PASS+1)); else echo "FAIL  $1 (got '$2', want '$3')"; FAIL=$((FAIL+1)); fi; }
has(){ if grep -q "$2" "$3" 2>/dev/null; then echo "PASS  $1"; PASS=$((PASS+1)); else echo "FAIL  $1 (not found in $3)"; FAIL=$((FAIL+1)); fi; }
hasnt(){ if grep -q "$2" "$3" 2>/dev/null; then echo "FAIL  $1 (found in $3)"; FAIL=$((FAIL+1)); else echo "PASS  $1"; PASS=$((PASS+1)); fi; }

echo "--- 1. the page carries the switch ---"
curl -s "$URL" > "$FH/page.html"
has "page: Keep it on section" "Keep it on" "$FH/page.html"
has "page: Turn on button"     'id="onbtn"'  "$FH/page.html"
has "page: Turn off button"    'id="offbtn"' "$FH/page.html"
has "page: state line filled"  "the tool pins only the games it launches itself" "$FH/page.html"
has "page: reboot checkbox"    'id="persist"' "$FH/page.html"

echo "--- 2. turn ON (this session only) ---"
R=$(curl -s -X POST -d "on=1&persist=0" "$URL""api/toggle")
echo "$R" | head -c 400; echo
has "ON: manager carries the pin" "MESA_VK_DEVICE_SELECT=" "$FH/manager.env"
hasnt "ON: nothing written for reboot" "MESA_VK_DEVICE_SELECT" "$FH/.config/environment.d/90-spirit.conf"

echo "--- 3. turn ON again WITH persist ---"
curl -s -X POST -d "on=1&persist=1" "$URL""api/toggle" > /dev/null
has "ON+persist: file written" "MESA_VK_DEVICE_SELECT=" "$FH/.config/environment.d/90-spirit.conf"

echo "--- 4. turn OFF ---"
R=$(curl -s -X POST -d "on=0&persist=0" "$URL""api/toggle")
echo "$R" | head -c 400; echo
hasnt "OFF: manager no longer carries the pin" "MESA_VK_DEVICE_SELECT" "$FH/manager.env"
if [ -e "$FH/.config/environment.d/90-spirit.conf" ]; then echo "FAIL  OFF: file still there"; FAIL=$((FAIL+1)); else echo "PASS  OFF: file removed"; PASS=$((PASS+1)); fi

echo "--- 5. the machine we are actually on was not touched ---"
env PATH="/usr/bin:/bin" systemctl --user show-environment > "$FH/real-after" 2>/dev/null || true
if diff -q "$FH/real-before" "$FH/real-after" >/dev/null 2>&1; then
  echo "PASS  the real user session is byte-identical before/after"; PASS=$((PASS+1))
else
  echo "FAIL  the real user session CHANGED"; FAIL=$((FAIL+1)); diff "$FH/real-before" "$FH/real-after"
fi

kill $UIPID 2>/dev/null; wait $UIPID 2>/dev/null
echo "================  $PASS passed, $FAIL failed  ================"
