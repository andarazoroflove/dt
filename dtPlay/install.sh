#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtplay" "$HOME/.local/bin/dtplay"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtplay1.fp" "$HOME/.dt/types/fp_dynamic/dtplay1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtplay.dt.in" > "$HOME/.dt/types/dtplay.dt"
mkdir -p "$HOME/.local/share/applications"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtplay.desktop.in" > "$HOME/.local/share/applications/dtplay.desktop"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtPlay -> $HOME/.local/bin/dtplay"
