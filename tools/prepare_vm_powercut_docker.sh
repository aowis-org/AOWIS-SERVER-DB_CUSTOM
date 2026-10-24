#!/usr/bin/env bash
# Prepare the reusable QEMU qualification container without starting a VM.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
exec "$script_dir/../qualify_vm_powercut_docker.sh" --prepare "$@"
