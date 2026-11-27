#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtdecoder" "$HOME/.local/bin/dtdecoder"
ln -sf "$HOME/.local/bin/dtdecoder" "$HOME/.local/bin/dtDecoder"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtdecoder1.fp" "$HOME/.dt/types/fp_dynamic/dtdecoder1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtdecoder.dt.in" > "$HOME/.dt/types/dtdecoder.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtDecoder -> $HOME/.local/bin/dtdecoder"
