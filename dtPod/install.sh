#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtpod" "$HOME/.local/bin/dtpod"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtpod1.fp" "$HOME/.dt/types/fp_dynamic/dtpod1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtpod.dt.in" > "$HOME/.dt/types/dtpod.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtPod -> $HOME/.local/bin/dtpod"
