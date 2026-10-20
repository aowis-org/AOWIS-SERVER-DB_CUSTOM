#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "${0##*/}" >&2
    exit 2
fi

model_path='external/AOWIS-SERVER-MODEL'
model_url="$(git config -f .gitmodules --get 'submodule.external/AOWIS-SERVER-MODEL.url' || true)"
if [[ -z "$model_url" ]]; then
    printf 'Missing Model URL in .gitmodules\n' >&2
    exit 1
fi

# A .gitmodules entry is only configuration; Git also needs a 160000 gitlink
# entry in the superproject's index. Fresh repositories may have only the former.
if ! git ls-files --stage -- "$model_path" | grep -q '^160000 '; then
    if [[ -e "$model_path" ]]; then
        printf 'Cannot register Model submodule: %s already exists without a gitlink.\n' "$model_path" >&2
        printf 'Move that path out of the way, then retry.\n' >&2
        exit 1
    fi
    printf 'Registering previously unpinned Model submodule from %s\n' "$model_url"
    git submodule add -- "$model_url" "$model_path"
    printf '\nIMPORTANT: First-time registration pinned the current Model commit.\n'
    printf 'Review and commit the gitlink in this repository:\n'
    printf '  git add .gitmodules %s\n' "$model_path"
    printf '  git commit -m "Register AOWIS Model submodule"\n\n'
fi

git submodule sync -- "$model_path"
git submodule update --init --recursive -- "$model_path"
printf 'Initialized pinned Model submodule at %s\n' "$model_path"
