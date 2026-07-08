#!/usr/bin/env bash
# ============================================================
# MiniLang test script -- run already-built tests (no build step)
# ------------------------------------------------------------
# Usage:
#   ./scripts/run_tests_only.sh            Run Debug tests via CTest
#   ./scripts/run_tests_only.sh release    Run Release tests via CTest
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

# --- Build type ---
BUILD_TYPE="${1:-debug}"
BUILD_TYPE="${BUILD_TYPE,,}"

# --- Detect environment ---
detect_platform
select_preset "$BUILD_TYPE"

BUILD_DIR="$(get_build_dir "$BUILD_TYPE")"

cd "$PROJECT_ROOT/$BUILD_DIR"

# Check test binary exists
if [ ! -f "tests/minilang_tests" ] && [ ! -f "tests/minilang_tests.exe" ]; then
    echo "[ERROR] minilang_tests not found. Build tests first."
    exit 1
fi

echo "[INFO] Running tests via CTest ($BUILD_TYPE)..."
ctest --output-on-failure
