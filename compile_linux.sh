#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

case "${1:-}" in
    '') [[ $# -eq 0 ]] || { printf 'Usage: %s [--build-only]\n' "${0##*/}" >&2; exit 2; } ;;
    --build-only) [[ $# -eq 1 ]] || { printf 'Usage: %s [--build-only]\n' "${0##*/}" >&2; exit 2; } ;;
    *) printf 'Usage: %s [--build-only]\n' "${0##*/}" >&2; exit 2 ;;
esac

build_dir="$project_dir/build-linux"
server="$build_dir/aowis-server-dvmdb"

# Explicit success gates matter when build-linux still contains a usable binary
# from an earlier build. Never fall through to that old server on a failure.
if cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release; then
    :
else
    status=$?
    printf 'DVMDB configuration failed (exit %d); server NOT started.\n' "$status" >&2
    exit "$status"
fi

# Build all Release utilities and benchmarks as before.
if cmake --build "$build_dir" --parallel; then
    :
else
    status=$?
    printf 'DVMDB build failed (exit %d); server NOT started.\n' "$status" >&2
    exit "$status"
fi

# Ensure this CMake configuration actually defines the HTTP server target.
# Otherwise an old executable left in build-linux could be launched despite
# the current configuration having HTTP disabled.
if cmake --build "$build_dir" --target aowis-server-dvmdb --parallel; then
    :
else
    status=$?
    printf 'DVMDB server target failed (exit %d); server NOT started.\n' "$status" >&2
    exit "$status"
fi

if [[ ! -f "$server" || ! -x "$server" ]]; then
    printf 'DVMDB build reported success, but the server executable is missing or not executable: %s\n' "$server" >&2
    exit 1
fi

printf 'AOWIS DVMDB Release binaries and benchmarks built in build-linux.\n'
if [[ "${1:-}" == --build-only ]]; then
    exit 0
fi

# Use the checked-out project's absolute binary, never ./build-linux relative
# to a different caller's current working directory.
printf 'Starting freshly verified DVMDB server: %s\n' "$server"
cd -- "$project_dir"
exec "$server"
