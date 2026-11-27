#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APPS=(
    dtMail
    dtCalendar
    dtTerm
    dtPod
    dtPaint
    dtPlay
    dtIRC
    dtPics
    dtNotes
    dtReader
    dtZip
    dtWine
    dtUpdate
    dtRadio
    dtScope
    dtDecoder
    dtRadar
    dtSat
    dtLog
    dtMaps
    dtDeck
    dtSys
    dtNet
    dtCalc
    dtDeluge
)

echo "=== Installing CDE 'dt' Application Suite into $HOME/.local/bin ==="
if [ -x "$ROOT_DIR/dtSubpanel/install.sh" ]; then
    echo "--> Installing dtSubpanel (multi-column dtwm subpanel engine)..."
    "$ROOT_DIR/dtSubpanel/install.sh"
fi
for app in "${APPS[@]}"; do
    echo "--> Installing $app..."
    "$ROOT_DIR/$app/install.sh"
done

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi

echo ""
echo "All 25 CDE applications installed! Restart dtwm (Workspace Menu -> Restart Workspace Manager)"
echo "to see all launchers in your Utilities, Mail, and Radio Apps Front Panel subpanels."
