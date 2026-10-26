#!/usr/bin/env bash
# Compatibility entrypoint for existing local workflows; canonical tool is zip_dvmdb.sh.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
exec "$script_dir/zip_dvmdb.sh" "$@"
