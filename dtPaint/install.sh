#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtpaint" "$HOME/.local/bin/dtpaint"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtpaint1.fp" "$HOME/.dt/types/fp_dynamic/dtpaint1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtpaint.dt.in" > "$HOME/.dt/types/dtpaint.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtPaint -> $HOME/.local/bin/dtpaint"
