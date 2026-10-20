#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s /path/to/aowis-db-custom-<description>.zip\n' "${0##*/}" >&2
    exit 2
fi

# Prevent cross-repository patch mixups. This repository accepts only patch
# archives specifically named for AOWIS-SERVER-DB_CUSTOM. Validate the archive
# basename, not its directory, before accessing or extracting any content.
patch_name="$(basename -- "$1")"
if [[ "$patch_name" != aowis-db-custom-*.zip ||
      "$patch_name" == aowis-db-custom-.zip ]]; then
    printf 'Refusing patch %s: expected aowis-db-custom-<description>.zip\n' "$patch_name" >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd -- "$script_dir/.." && pwd -P)"
parent="$(dirname -- "$project_root")"
project_name="$(basename -- "$project_root")"
patch="$(realpath -- "$1")"

# Only accept archives containing paths inside this repository.
while IFS= read -r entry; do
    if [[ "$entry" != "$project_name/"* || "$entry" == *'/../'* ||
          "$entry" == *'/./'* || "$entry" == *'\\'* ]]; then
        printf 'Invalid patch path: %s\n' "$entry" >&2
        exit 1
    fi
done < <(unzip -Z -1 "$patch")

unzip -q -o "$patch" -d "$parent"

# ZIP timestamps reflect archive creation, not application. Refresh both
# sources and directories here so Ninja sees changes when applying a patch.
while IFS= read -r entry; do
    touch -c -m -- "$parent/$entry"
done < <(unzip -Z -1 "$patch")

printf 'Applied %s with current file and directory timestamps.\n' "$patch"
