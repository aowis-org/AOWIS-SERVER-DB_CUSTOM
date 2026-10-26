#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "${0##*/}" >&2
    exit 2
fi

# Performance measurements must use an optimized build. The test helper
# maintains separate Debug and Release directories.
cmake -S "$project_dir" -B "$project_dir/build-linux" -G Ninja \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_dir/build-linux" --parallel
printf 'AOWIS native storage library and Release benchmark built in build-linux.\n'

./build-linux/aowis-server-dvmdb

