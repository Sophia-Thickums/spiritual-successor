#!/bin/sh
# Install Spiritual Successor — puts the `spirit` command on your PATH.
#
#   curl -fsSL https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main/install.sh | sh
#
# It downloads two Python files and makes one tiny wrapper. Nothing else — no packages, no root.
set -e

DIR="${SPIRIT_HOME:-$HOME/.local/share/spirit}"
BIN="${SPIRIT_BIN:-$HOME/.local/bin}"
BASE="https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main"

command -v python3 >/dev/null 2>&1 || { echo "python3 is required (3.8 or newer)." >&2; exit 1; }

mkdir -p "$DIR" "$BIN"
echo "Downloading spirit…"
curl -fsSL "$BASE/spirit.py" -o "$DIR/spirit.py"
# The local page. `spirit ui` looks for this beside spirit.py, so it belongs in the same folder.
curl -fsSL "$BASE/spirit_ui.py" -o "$DIR/spirit_ui.py"

printf '#!/bin/sh\nexec python3 "%s/spirit.py" "$@"\n' "$DIR" > "$BIN/spirit"
chmod +x "$BIN/spirit"

echo "Installed: $BIN/spirit"
case ":$PATH:" in
  *":$BIN:"*) : ;;
  *) echo
     echo "One thing left — put $BIN on your PATH:"
     echo "    echo 'export PATH=\"$BIN:\$PATH\"' >> ~/.profile && . ~/.profile" ;;
esac
echo
echo "Now run:    spirit doctor"
echo "Then try:   spirit ui          (a local page, if you would rather click than type)"
echo "       or:  spirit play -- ./yourgame"
