#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
make
make install
echo "Installed dtCalc -> $HOME/.local/bin/cde-calc"
