#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APPS=(
    dtMail
    dtCalendar
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
)

echo "=== Installing CDE 'dt' Application Suite into $HOME/.local/bin ==="
for app in "${APPS[@]}"; do
    echo "--> Installing $app..."
    "$ROOT_DIR/$app/install.sh"
done

if [ -x /usr/dt/bin/dtaction ]; then
    /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true
fi

echo ""
echo "All 12 CDE applications installed! Restart dtwm (Workspace Menu -> Restart Workspace Manager)"
echo "to see all launchers in your Utilities and Mail Front Panel subpanels."
