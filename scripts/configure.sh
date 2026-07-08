#!/usr/bin/env bash
# ============================================================
# MiniLang IDE CMake configure script (Linux/macOS)
# ------------------------------------------------------------
# Auto-detects Qt6 and runs cmake with the appropriate preset.
#
# Usage:
#   ./scripts/configure.sh              Configure Debug (default)
#   ./scripts/configure.sh release      Configure Release
#   ./scripts/configure.sh -- -DFOO=bar Pass extra args to cmake
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

# --- Parse arguments ---
BUILD_TYPE="debug"
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "${1,,}" in
        release) BUILD_TYPE="release"; shift;;
        debug)   BUILD_TYPE="debug"; shift;;
        --)      shift; EXTRA_ARGS=("$@"); break;;
        *)       EXTRA_ARGS+=("$1"); shift;;
    esac
done

# --- Detect environment ---
detect_platform
check_prerequisites
detect_qt
select_preset "$BUILD_TYPE"

# --- Run CMake ---
echo "[INFO] Running: cmake --preset $CMAKE_PRESET ${EXTRA_ARGS[*]:-}"
cmake --preset "$CMAKE_PRESET" "${EXTRA_ARGS[@]}"

echo "[INFO] CMake configuration done."
echo "[INFO] Build with: cmake --build $(get_build_dir "$BUILD_TYPE")"
