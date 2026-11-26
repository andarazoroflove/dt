#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtzip" "$HOME/.local/bin/dtzip"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtzip1.fp" "$HOME/.dt/types/fp_dynamic/dtzip1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtzip.dt.in" > "$HOME/.dt/types/dtzip.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtZip -> $HOME/.local/bin/dtzip"
