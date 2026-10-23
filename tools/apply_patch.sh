#!/usr/bin/env bash
set -euo pipefail

# Patches may replace this very script while it runs, and unzip rewrites
# files in place. bash reads scripts incrementally, so all work happens in
# main(), which bash parses completely before the first command executes.
main() {
    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
    project_root="$(cd -- "$script_dir/.." && pwd -P)"
    # Patch archives may include an optional deletion manifest. A prior copy of
    # this script does not know about it; after updating the script, run --finish
    # once to remove the listed obsolete files. Newer applications finalize it
    # automatically. The manifest is transient and is never kept in the worktree.
    manifest="$project_root/.aowis-patch-delete.txt"

    apply_deletions() {
        [[ ! -L "$manifest" ]] || { echo "Refusing symlinked patch deletion manifest" >&2; return 1; }
        [[ -f "$manifest" ]] || return 0
        local relative path segment
        local -a entries=() segments=()
        # Validate the *entire* manifest before deleting anything.
        while IFS= read -r relative || [[ -n "$relative" ]]; do
            [[ -n "$relative" && "$relative" != /* && "$relative" != .* &&
               "$relative" != *'\'* ]] || {
                printf 'Unsafe patch deletion path: %s\n' "$relative" >&2
                return 1
            }
            IFS='/' read -r -a segments <<< "$relative"
            for segment in "${segments[@]}"; do
                [[ -n "$segment" && "$segment" != . && "$segment" != .. &&
                   "$segment" != .git ]] || {
                    printf 'Unsafe patch deletion path: %s\n' "$relative" >&2
                    return 1
                }
            done
            path="$(realpath -m -- "$project_root/$relative")"
            [[ "$path" == "$project_root/"* && ! -d "$path" ]] || {
                printf 'Patch deletion escapes repository or names a directory: %s\n' "$relative" >&2
                return 1
            }
            entries+=("$path")
        done < "$manifest"
        for path in "${entries[@]}"; do
            rm -f -- "$path"
        done
        rm -f -- "$manifest"
        printf 'Removed %d obsolete file(s) listed by patch.\n' "${#entries[@]}"
    }

    if [[ $# -eq 1 && "$1" == "--finish" ]]; then
        [[ -f "$manifest" ]] || { echo 'No pending patch deletions.'; exit 0; }
        apply_deletions
        exit 0
    fi
    if [[ $# -ne 1 ]]; then
        printf 'Usage: %s /path/to/aowis-db-custom-<description>.zip | --finish\n' "${0##*/}" >&2
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

    apply_deletions
    printf 'Applied %s with current file and directory timestamps.\n' "$patch"

    # Refresh the project snapshot (db_custom.zip next to the repository) so
    # the applied state can be shared for review without a separate step.
    if ! "$script_dir/zip_db.sh"; then
        printf 'Patch applied, but creating the project snapshot failed.\n' >&2
        exit 1
    fi
}

main "$@"; exit $?
