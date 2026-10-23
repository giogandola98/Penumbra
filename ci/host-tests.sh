#!/bin/bash
# Run host-side unit tests (tests/host), if present.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [ -d "$ROOT/tests/host" ]; then
    make -C "$ROOT/tests/host"
else
    echo "no host tests yet"
fi
