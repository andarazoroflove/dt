#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtdeck" "$HOME/.local/bin/dtdeck"
ln -sf "$HOME/.local/bin/dtdeck" "$HOME/.local/bin/dtDeck"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtdeck1.fp" "$HOME/.dt/types/fp_dynamic/dtdeck1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtdeck.dt.in" > "$HOME/.dt/types/dtdeck.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtDeck -> $HOME/.local/bin/dtdeck"
