#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtlog" "$HOME/.local/bin/dtlog"
ln -sf "$HOME/.local/bin/dtlog" "$HOME/.local/bin/dtLog"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtlog1.fp" "$HOME/.dt/types/fp_dynamic/dtlog1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtlog.dt.in" > "$HOME/.dt/types/dtlog.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtLog -> $HOME/.local/bin/dtlog"
