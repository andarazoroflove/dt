#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtsys" "$HOME/.local/bin/dtsys"
ln -sf "$HOME/.local/bin/dtsys" "$HOME/.local/bin/dtSys"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtsys1.fp" "$HOME/.dt/types/fp_dynamic/dtsys1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtsys.dt.in" > "$HOME/.dt/types/dtsys.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtSys -> $HOME/.local/bin/dtsys"
