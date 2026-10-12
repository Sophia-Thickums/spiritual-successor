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

# ★ A LAUNCHER, so the page is one click instead of a command line. Without this the tool is a GUI you
# can only reach by typing, which is the thing it exists to avoid. Written for the current user only
# (no root, no system directory) and points at the wrapper by absolute path, because a desktop session
# does not necessarily have ~/.local/bin on its PATH.
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
cat > "$APPS/spirit.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Spiritual Successor
GenericName=Multi-GPU offload
Comment=Play on the card your monitor is on. Your other cards do the work beside you.
Exec=$BIN/spirit ui
Icon=video-display
Terminal=false
Categories=Utility;
Keywords=GPU;offload;encode;transcode;multi-GPU;
StartupNotify=true
DESKTOP
chmod 644 "$APPS/spirit.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" >/dev/null 2>&1

echo "Installed: $BIN/spirit"
echo "Launcher:  $APPS/spirit.desktop  (find 'Spiritual Successor' in your applications menu)"
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
echo "       or:  spirit enable     (arm the pin for every launch from now on, Steam included; nothing at boot)"
