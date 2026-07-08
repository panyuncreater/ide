#!/usr/bin/env bash
# ============================================================
# MiniLang IDE build script (Linux/macOS)
# ------------------------------------------------------------
# Auto-detects Qt6, configures if needed, and builds the IDE.
#
# Usage:
#   ./scripts/build.sh            Build Debug (default)
#   ./scripts/build.sh release    Build Release
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

# --- Build ---
echo "[INFO] Building IDE ($BUILD_TYPE)..."
cmake --build "$PROJECT_ROOT/$BUILD_DIR" --target minilang_ide --parallel

echo "[INFO] Build OK: $BUILD_DIR/minilang_ide"
