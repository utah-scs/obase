#!/usr/bin/env bash
# Alias for the shared collector; see tools/measurements/README.md.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$script_dir/measure.py" capture --profiles samples "$@"
