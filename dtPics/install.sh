#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtpics" "$HOME/.local/bin/dtpics"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtpics1.fp" "$HOME/.dt/types/fp_dynamic/dtpics1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtpics.dt.in" > "$HOME/.dt/types/dtpics.dt"
mkdir -p "$HOME/.local/share/applications"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtpics.desktop.in" > "$HOME/.local/share/applications/dtpics.desktop"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtPics -> $HOME/.local/bin/dtpics"
