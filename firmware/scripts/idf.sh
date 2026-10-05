#!/usr/bin/env bash
# Run the pinned IDF toolchain from any working directory.
set -euo pipefail
firmware_dir=$(cd "$(dirname "$0")/.." && pwd)
if [[ -z "${IDF_PATH:-}" ]]; then
  for candidate in "$HOME/esp/esp-idf-v6.0.1" "$HOME/esp/esp-idf-v6" /private/tmp/bunty-esp-idf-6.0.1; do
    if [[ -f "$candidate/export.sh" ]]; then
      export IDF_PATH="$candidate"
      break
    fi
  done
fi
if [[ ! -f "${IDF_PATH:-}/export.sh" ]]; then
  echo 'Install ESP-IDF 6.0.1 and set IDF_PATH; see firmware/README.md.' >&2
  exit 1
fi
if [[ "$IDF_PATH" == /private/tmp/bunty-esp-idf-6.0.1 ]]; then
  export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-/private/tmp/bunty-idf-tools}
  # Use the interpreter installed with this temporary toolchain, rather than
  # whichever Python a per-directory development environment selects.
  for candidate in "$IDF_TOOLS_PATH"/python_env/idf6.0_py*_env/bin; do
    if [[ -x "$candidate/python" ]]; then
      export PATH="$candidate:$PATH"
      break
    fi
  done
fi
# ESP-IDF's activation script handles its own unset variables.
set +u
source "$IDF_PATH/export.sh" >/dev/null
set -u
cd "$firmware_dir"
if [[ $# -eq 0 ]]; then set -- build; fi
exec idf.py "$@"
