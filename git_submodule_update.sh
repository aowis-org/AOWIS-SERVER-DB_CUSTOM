#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
    cat <<'HELP'
Usage: ./git_submodule_update.sh [--help]

Fetch the Model's configured remote branch and move the submodule gitlink to
its current commit. Review, test, and commit the updated gitlink in this DB
repository if you intend to keep the new version.

To restore the currently pinned revision without advancing it, use
./git_submodule_init.sh instead.
HELP
}

case "${1:-}" in
    '') [[ $# -eq 0 ]] || { usage >&2; exit 2; } ;;
    --help|-h) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

model_path="external/AOWIS-SERVER-MODEL"
if [[ "$(git ls-files --stage -- "$model_path")" != '160000 '* ]]; then
    printf 'Model submodule is not registered. Run ./git_submodule_init.sh first.\n' >&2
    exit 1
fi

git submodule sync -- "$model_path"
git submodule update --init --remote -- "$model_path"
printf '\nReview and commit the updated Model gitlink, if intended:\n'
printf '  git diff --submodule=log -- %s\n' "$model_path"
printf '  git add %s && git commit -m "Update AOWIS Model submodule"\n' "$model_path"
