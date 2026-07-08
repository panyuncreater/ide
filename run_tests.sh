#!/usr/bin/env bash
# Wrapper: delegates to scripts/run_tests.sh
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/run_tests.sh" "$@"
