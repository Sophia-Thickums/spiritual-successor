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
HICOLOR="$DATA/icons/hicolor"
BASE="https://raw.githubusercontent.com/Sophia-Thickums/spiritual-successor/main"

command -v python3 >/dev/null 2>&1 || { echo "python3 is required (3.8 or newer)." >&2; exit 1; }

mkdir -p "$DIR" "$BIN" "$APPS" "$HICOLOR"
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
   && pkg-config --exists gtk4 2>/dev/null && pkg-config --exists libadwaita-1 2>/dev/null; then
  echo "Building the desktop app… (the first build takes a few minutes)"
  TMPGUI="$(mktemp -d)"
  # ★ A TRAP, so an interrupted build does not leave a few hundred MB of cargo target in /tmp.
  #   ★ INT/TERM MUST EXIT (outside read, 2026-10-12): a trap that only cleans up lets Ctrl-C kill
  #   cargo and then the install CARRY ON as if nothing happened.
  trap 'rm -rf "$TMPGUI"' EXIT
  trap 'rm -rf "$TMPGUI"; exit 130' INT TERM
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
  echo "Desktop app: NOT built — it needs cargo and the GTK4 + libadwaita development headers."
  echo "             Arch/CachyOS:   sudo pacman -S rust gtk4 libadwaita"
  echo "             Debian/Ubuntu:  sudo apt install cargo libgtk-4-dev libadwaita-1-dev"
  echo "             Everything below still works without it."
fi

# ---------------------------------------------------------------------------------------------
# THE ICON.
# ★ IT SHIPS PICTURES, NOT A THEME INDEX. The first version of this downloaded the upstream
#   `hicolor/index.theme` — 55 KB, the fallback theme index for the whole desktop — and wrote it to
#   the user's own copy. That would have overwritten theirs and quietly degraded icon resolution for
#   EVERY app on their machine, not just this one. An application drops its icons into the standard
#   folder and lets the system's index cover them; it does not own `hicolor`.
#   The PNGs at the standard sizes are the part that matters: KDE draws icons with Qt's SVG renderer
#   and expects pictures already sized, so an SVG alone can leave a blank square.
# ---------------------------------------------------------------------------------------------
echo "Installing the icon…"
mkdir -p "$HICOLOR"
for SZ in 16 22 24 32 48 64 128 256; do
  mkdir -p "$HICOLOR/${SZ}x${SZ}/apps"
  curl -fsSL "$BASE/gui/assets/icons/hicolor/${SZ}x${SZ}/apps/spirit.png" \
       -o "$HICOLOR/${SZ}x${SZ}/apps/spirit.png" 2>/dev/null || true
done
mkdir -p "$HICOLOR/scalable/apps"
curl -fsSL "$BASE/gui/assets/icons/hicolor/scalable/apps/spirit.svg" \
     -o "$HICOLOR/scalable/apps/spirit.svg" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$HICOLOR" >/dev/null 2>&1 || true

# The launcher: the desktop app when we have it, the page when we do not. Never a dead entry.
if [ -x "$GUI" ]; then EXEC="$GUI"; NOTIFY=true; else EXEC="$BIN/spirit ui"; NOTIFY=false; fi
write_entry() {   # $1 = file to write, $2 = extra lines
  cat > "$1" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Spiritual Successor
GenericName=Multi-GPU offload
Comment=Play on the card your monitor is on; give your other GPUs a job when you ask.
Exec=$EXEC
Icon=spirit
Terminal=false
Categories=System;
Keywords=GPU;offload;encode;transcode;multi-GPU;
StartupNotify=$NOTIFY
$2
DESKTOP
  chmod 644 "$1"
}
write_entry "$APPS/com.sophia.SpiritualSuccessor.desktop" ""

# ★ A SECOND ENTRY, HIDDEN, NAMED AFTER THE PROGRAM ITSELF. A window's task-bar icon is looked up by
#   whatever name the window reports for itself — the reverse-domain name for some toolkits, the
#   program name for others. This one carries the same icon and `NoDisplay=true`, so it is invisible in
#   the menu but gives the task bar something to match either way. Written out in full rather than
#   generated by a text substitution, so a later edit to the entry above cannot silently break it.
#   StartupNotify is false on the page fallback: that path never sends the "I have started" signal, and
#   leaving it true spins the launching cursor forever.
write_entry "$APPS/spirit-gui.desktop" "NoDisplay=true"

# ★ `|| true`: under `set -e` a nonzero from update-desktop-database would ABORT the script here
#   (outside read, 2026-10-12), silently skipping the PATH instructions below. A missing menu index
#   is not a reason to fail an install.
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" >/dev/null 2>&1 || true

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
