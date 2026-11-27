#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtradio" "$HOME/.local/bin/dtradio"
ln -sf "$HOME/.local/bin/dtradio" "$HOME/.local/bin/dtRadio"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtradio1.fp" "$HOME/.dt/types/fp_dynamic/dtradio1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtradio.dt.in" > "$HOME/.dt/types/dtradio.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtRadio -> $HOME/.local/bin/dtradio"
