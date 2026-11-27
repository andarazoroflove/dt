#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtnet" "$HOME/.local/bin/dtnet"
ln -sf "$HOME/.local/bin/dtnet" "$HOME/.local/bin/dtNet"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtnet1.fp" "$HOME/.dt/types/fp_dynamic/dtnet1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtnet.dt.in" > "$HOME/.dt/types/dtnet.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtNet -> $HOME/.local/bin/dtnet"
