#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

make
make install

# Persist Dtsession*wmStartupCommand in ~/.dt/sessions/current/dt.resources
if [ -f "$HOME/.dt/sessions/current/dt.resources" ]; then
    if ! grep -q '^Dtsession\*wmStartupCommand:' "$HOME/.dt/sessions/current/dt.resources" 2>/dev/null; then
        printf '\nDtsession*wmStartupCommand:\t%s/.local/bin/dtwm-wrapper\n' "$HOME" >> "$HOME/.dt/sessions/current/dt.resources"
    else
        sed -i "s|^Dtsession\*wmStartupCommand:.*|Dtsession*wmStartupCommand:\t$HOME/.local/bin/dtwm-wrapper|" "$HOME/.dt/sessions/current/dt.resources"
    fi
fi

if [ -w "$HOME/.Xdefaults" ] || [ -w "$HOME" ]; then
    touch "$HOME/.Xdefaults" 2>/dev/null || true
    if [ -w "$HOME/.Xdefaults" ]; then
        if ! grep -q '^Dtsession\*wmStartupCommand:' "$HOME/.Xdefaults" 2>/dev/null; then
            printf '\nDtsession*wmStartupCommand:\t%s/.local/bin/dtwm-wrapper\n' "$HOME" >> "$HOME/.Xdefaults"
        else
            sed -i "s|^Dtsession\*wmStartupCommand:.*|Dtsession*wmStartupCommand:\t$HOME/.local/bin/dtwm-wrapper|" "$HOME/.Xdefaults"
        fi
    fi
fi

echo "Installed libdtwm_subpanel_cols.so and dtwm-wrapper"
