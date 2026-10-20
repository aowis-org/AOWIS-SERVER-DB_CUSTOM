#!/usr/bin/env bash
set -euo pipefail

# Package the current project, including uncommitted and newly added files,
# without bundling .git or ignored/generated build artifacts.
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

output_arg="${1:-$project_parent/db_custom.zip}"
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

# Build the archive outside the worktree. Move it into place only on success.
tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "$tmp_dir"' EXIT
tmp_archive="$tmp_dir/db.zip"

files=()
while IFS= read -r -d '' relative_path; do
    [[ -f "$project_root/$relative_path" ]] || continue

    # Never package private environment files or key material by accident.
    case "/$relative_path" in
        */.env|*/.env.*|*.pem|*.key|*.p12|*.pfx) continue ;;
    esac

    [[ "$project_root/$relative_path" != "$output_path" ]] || continue

    if [[ "$relative_path" == *$'\n'* ]]; then
        printf 'Error: filenames containing newlines cannot be archived.\n' >&2
        exit 1
    fi

    files+=("$project_name/$relative_path")
done < <(git -C "$project_root" ls-files -z --cached --others --exclude-standard)

# Include the checked-out Model submodule sources: the superproject index
# contains only a gitlink, so the loop above intentionally skips its tree.
model_rel="external/AOWIS-SERVER-MODEL"
if [[ "$(git -C "$project_root" ls-files --stage -- "$model_rel")" == '160000 '* ]]; then
    model_root="$project_root/$model_rel"
    if [[ ! -f "$model_root/CMakeLists.txt" ]] ||
       [[ "$(git -C "$model_root" rev-parse --show-toplevel 2>/dev/null)" != "$model_root" ]]; then
        printf 'Error: Model submodule not initialized. Run ./git_submodule_init.sh first.\n' >&2
        exit 1
    fi
    while IFS= read -r -d '' relative_path; do
        [[ -f "$model_root/$relative_path" ]] || continue
        case "/$relative_path" in
            */.env|*/.env.*|*.pem|*.key|*.p12|*.pfx) continue ;;
        esac
        if [[ "$relative_path" == *$'\n'* ]]; then
            printf 'Error: filenames containing newlines cannot be archived.\n' >&2
            exit 1
        fi
        files+=("$project_name/$model_rel/$relative_path")
    done < <(git -C "$model_root" ls-files -z --cached --others --exclude-standard)
fi

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
