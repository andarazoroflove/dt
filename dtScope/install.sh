#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$HOME/.local/bin" "$HOME/.dt/icons" "$HOME/.dt/types/fp_dynamic"

install -m 755 "$SCRIPT_DIR/dtscope" "$HOME/.local/bin/dtscope"
ln -sf "$HOME/.local/bin/dtscope" "$HOME/.local/bin/dtScope"
cp -f "$SCRIPT_DIR"/icons/*.pm "$HOME/.dt/icons/"
cp -f "$SCRIPT_DIR/cde/dtscope1.fp" "$HOME/.dt/types/fp_dynamic/dtscope1.fp"
sed "s|__HOME__|$HOME|g" "$SCRIPT_DIR/cde/dtscope.dt.in" > "$HOME/.dt/types/dtscope.dt"

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi
echo "Installed dtScope -> $HOME/.local/bin/dtscope"
