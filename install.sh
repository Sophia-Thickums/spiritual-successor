#!/bin/sh
# Install Spiritual Successor — the command, the desktop app, and a launcher for both.
#
#   curl -fsSL https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main/install.sh | sh
#
# No root. The command is two Python files; the desktop app is built from source on your own machine.
set -e

DIR="${SPIRIT_HOME:-$HOME/.local/share/spirit}"
BIN="${SPIRIT_BIN:-$HOME/.local/bin}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS="$DATA/applications"
ICONS="$DATA/icons/hicolor/scalable/apps"
THEME="$DATA/icons/hicolor"
BASE="https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main"

command -v python3 >/dev/null 2>&1 || { echo "python3 is required (3.8 or newer)." >&2; exit 1; }

mkdir -p "$DIR" "$BIN" "$APPS" "$ICONS"
echo "Downloading spirit…"
curl -fsSL "$BASE/spirit.py" -o "$DIR/spirit.py"
# The local page. `spirit ui` looks for this beside spirit.py, so it belongs in the same folder.
curl -fsSL "$BASE/spirit_ui.py" -o "$DIR/spirit_ui.py"

printf '#!/bin/sh\nexec python3 "%s/spirit.py" "$@"\n' "$DIR" > "$BIN/spirit"
chmod +x "$BIN/spirit"

# ---------------------------------------------------------------------------------------------
# THE NATIVE DESKTOP APP — GTK4 + libadwaita, the same shape as the GPU tool this sits beside.
# ★ IT IS BUILT HERE RATHER THAN DOWNLOADED AS A BINARY, deliberately: a prebuilt binary in the repo
#   would be one architecture pretending to be all of them. And if the toolchain is missing this says
#   so in plain words — a silent skip would leave the launcher pointing at nothing, which is exactly
#   the "opens the thing you were told to replace" failure this replaced.
# ---------------------------------------------------------------------------------------------
GUI="$BIN/spirit-gui"
if command -v cargo >/dev/null 2>&1 && command -v pkg-config >/dev/null 2>&1 \
   && pkg-config --exists gtk4 2>/dev/null; then
  echo "Building the desktop app… (the first build takes a few minutes)"
  TMPGUI="$(mktemp -d)"
  # ★ A TRAP, so an interrupted build does not leave a few hundred MB of cargo target in /tmp.
  trap 'rm -rf "$TMPGUI"' EXIT INT TERM
  mkdir -p "$TMPGUI/src"
  curl -fsSL "$BASE/gui/Cargo.toml"  -o "$TMPGUI/Cargo.toml"
  curl -fsSL "$BASE/gui/src/main.rs" -o "$TMPGUI/src/main.rs"
  if (cd "$TMPGUI" && cargo build --release --quiet); then
    cp "$TMPGUI/target/release/spirit-gui" "$GUI"
    chmod +x "$GUI"
    echo "Installed: $GUI"
  else
    echo "The desktop app did not build. The command and the local page still work."
  fi
  rm -rf "$TMPGUI"
else
  echo
  echo "Desktop app: NOT built — it needs cargo and the GTK4 development headers."
  echo "             Arch/CachyOS:   sudo pacman -S rust gtk4 libadwaita"
  echo "             Debian/Ubuntu:  sudo apt install cargo libgtk-4-dev libadwaita-1-dev"
  echo "             Everything below still works without it."
fi

# The icon, so the launcher is not a blank square.
curl -fsSL "$BASE/gui/assets/spirit.svg" -o "$ICONS/spirit.svg" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$THEME" >/dev/null 2>&1 || true

# The launcher: the desktop app when we have it, the page when we do not. Never a dead entry.
if [ -x "$GUI" ]; then EXEC="$GUI"; else EXEC="$BIN/spirit ui"; fi
cat > "$APPS/com.sophia.SpiritualSuccessor.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Spiritual Successor
GenericName=Multi-GPU offload
Comment=Play on the card your monitor is on. Your other cards do the work beside you.
Exec=$EXEC
Icon=spirit
Terminal=false
Categories=System;
Keywords=GPU;offload;encode;transcode;multi-GPU;
StartupNotify=true
DESKTOP
chmod 644 "$APPS/com.sophia.SpiritualSuccessor.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" >/dev/null 2>&1

echo "Launcher:  $APPS/com.sophia.SpiritualSuccessor.desktop"
echo "           (find 'Spiritual Successor' in your applications menu)"
case ":$PATH:" in
  *":$BIN:"*) : ;;
  *) echo
     echo "One thing left — put $BIN on your PATH:"
     echo "    echo 'export PATH=\"$BIN:\$PATH\"' >> ~/.profile && . ~/.profile" ;;
esac
echo
echo "Now run:    spirit doctor"
echo "Then try:   spirit play -- ./yourgame"
echo "       or:  spirit enable     (arm the pin for every launch from now on, Steam included; nothing at boot)"
