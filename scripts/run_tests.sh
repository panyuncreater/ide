#!/usr/bin/env bash
# ============================================================
# MiniLang IDE test script (Linux/macOS)
# ------------------------------------------------------------
# Auto-detects Qt6, builds tests, and runs them via CTest.
#
# Usage:
#   ./scripts/run_tests.sh            Build and test Debug (default)
#   ./scripts/run_tests.sh release    Build and test Release
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

# --- Build type ---
BUILD_TYPE="${1:-debug}"
BUILD_TYPE="${BUILD_TYPE,,}"  # lowercase

# --- Detect environment ---
detect_platform
check_prerequisites
detect_qt
select_preset "$BUILD_TYPE"

BUILD_DIR="$(get_build_dir "$BUILD_TYPE")"

# --- Configure if needed ---
if [ ! -f "$PROJECT_ROOT/$BUILD_DIR/build.ninja" ] &&    [ ! -f "$PROJECT_ROOT/$BUILD_DIR/Makefile" ] &&    [ ! -f "$PROJECT_ROOT/$BUILD_DIR/CMakeCache.txt" ]; then
    echo "[INFO] Configuring with configure.sh $BUILD_TYPE..."
    "$SCRIPT_DIR/configure.sh" "$BUILD_TYPE"
fi

# --- Build tests ---
echo "[INFO] Building tests ($BUILD_TYPE)..."
cmake --build "$PROJECT_ROOT/$BUILD_DIR" --target minilang_tests --parallel

# --- Run tests ---
echo "[INFO] Running tests via CTest..."
cd "$PROJECT_ROOT/$BUILD_DIR"
ctest --output-on-failure --no-compress-output
