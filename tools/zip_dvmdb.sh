#!/usr/bin/env bash
set -euo pipefail

# Package the complete checked-out project, including changes and non-ignored
# untracked files in every initialized submodule (and nested submodules).
# Gitlinks contain commit IDs, not source files; recurse into each worktree.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd -- "$script_dir/.." && pwd -P)"
project_parent="$(dirname -- "$project_root")"
project_name="$(basename -- "$project_root")"

if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [output.zip]\n' "${0##*/}" >&2
    exit 2
fi

if ! command -v git >/dev/null 2>&1 || ! command -v zip >/dev/null 2>&1; then
    printf 'Error: git and zip must be installed.\n' >&2
    exit 1
fi

if [[ "$(git -C "$project_root" rev-parse --show-toplevel 2>/dev/null)" != "$project_root" ]]; then
    printf 'Error: %s must be the root of a Git worktree.\n' "$project_root" >&2
    exit 1
fi

output_arg="${1:-$project_parent/dvmdb.zip}"
output_dir="$(dirname -- "$output_arg")"
if [[ ! -d "$output_dir" ]]; then
    printf 'Error: output directory does not exist: %s\n' "$output_dir" >&2
    exit 1
fi
output_path="$(cd -- "$output_dir" && pwd -P)/$(basename -- "$output_arg")"

if [[ "$output_path" != *.zip ]]; then
    printf 'Error: output filename must end in .zip.\n' >&2
    exit 1
fi

# Create the archive outside the worktree and replace the destination only
# after successful discovery and packaging. A missing submodule must not leave
# behind an apparently complete snapshot.
tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "$tmp_dir"' EXIT
tmp_archive="$tmp_dir/db.zip"
files=()

collect_worktree() {
    local worktree="$1"
    local prefix="$2"  # Path relative to the project root; empty at the top.
    local file_list index_list relative_path project_path entry metadata mode stage
    local submodule_root submodule_path git_root

    # Check the command's exit status: failures inside process substitutions
    # would otherwise be masked by the surrounding while loop.
    file_list="$(mktemp "$tmp_dir/files.XXXXXX")"
    git -C "$worktree" ls-files -z --cached --others --exclude-standard > "$file_list"
    while IFS= read -r -d '' relative_path; do
        [[ -f "$worktree/$relative_path" ]] || continue
        project_path="${prefix:+$prefix/}$relative_path"

        # Never package private environment files or key material by accident.
        case "/$project_path" in
            */.env|*/.env.*|*.pem|*.key|*.p12|*.pfx|*/__pycache__/*|*.pyc|*.pyo) continue ;;
        esac
        if [[ "$project_path" == *$'\n'* ]]; then
            printf 'Error: filenames containing newlines cannot be archived.\n' >&2
            return 1
        fi
        [[ "$worktree/$relative_path" != "$output_path" ]] || continue
        files+=("$project_name/$project_path")
    done < "$file_list"

    # Discover every gitlink from the index, rather than relying on hardcoded
    # submodule names or .gitmodules text (which alone does not pin a commit).
    index_list="$(mktemp "$tmp_dir/gitlinks.XXXXXX")"
    git -C "$worktree" ls-files --stage -z > "$index_list"
    while IFS= read -r -d '' entry; do
        metadata="${entry%%$'\t'*}"
        mode="${metadata%% *}"
        [[ "$mode" == 160000 ]] || continue
        stage="${metadata##* }"
        relative_path="${entry#*$'\t'}"
        submodule_path="${prefix:+$prefix/}$relative_path"
        if [[ "$stage" != 0 || "$submodule_path" == *$'\n'* ]]; then
            printf 'Error: unresolved or invalid submodule gitlink: %s\n' "$submodule_path" >&2
            return 1
        fi

        submodule_root="$worktree/$relative_path"
        # Without its own Git worktree, `git -C` could silently fall back to
        # the parent repo and cause a partial or incorrect ZIP.
        git_root="$(git -C "$submodule_root" rev-parse --show-toplevel 2>/dev/null)" || {
            printf 'Error: submodule %s is not initialized; run git submodule update --init --recursive.\n' "$submodule_path" >&2
            return 1
        }
        if [[ "$(cd -- "$git_root" && pwd -P)" != "$(cd -- "$submodule_root" && pwd -P)" ]]; then
            printf 'Error: submodule %s is not initialized; run git submodule update --init --recursive.\n' "$submodule_path" >&2
            return 1
        fi
        collect_worktree "$submodule_root" "$submodule_path"
    done < "$index_list"
}

collect_worktree "$project_root" ""

if [[ ${#files[@]} -eq 0 ]]; then
    printf 'Error: no project files found to archive.\n' >&2
    exit 1
fi

(
    cd -- "$project_parent"
    printf '%s\n' "${files[@]}" | zip -q "$tmp_archive" -@
)

mv -f -- "$tmp_archive" "$output_path"
printf 'Created %s (%d files)\n' "$output_path" "${#files[@]}"
