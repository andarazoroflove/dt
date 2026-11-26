#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
make
make install
echo "Installed dtTerm -> $HOME/.local/bin/dtTerm (and $HOME/.local/bin/dtterm)"
