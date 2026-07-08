#!/usr/bin/env bash
# Wrapper: delegates to scripts/configure.sh
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/configure.sh" "$@"
