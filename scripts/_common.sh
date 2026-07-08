#!/usr/bin/env bash
# ============================================================
# MiniLang build scripts -- shared header (platform detect + Qt6 detect)
# ------------------------------------------------------------
# Sourced by configure.sh / build.sh / run_tests.sh.
# After sourcing, PROJECT_ROOT, QTDIR, and CMAKE_PRESET are set.
# ============================================================

set -euo pipefail

# --- Locate project root (this script lives in scripts/) ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# --- Platform detection ---
detect_platform() {
    case "$(uname -s)" in
        Linux*)  PLATFORM="linux";;
        Darwin*) PLATFORM="macos";;
        *)       echo "[ERROR] Unsupported platform: $(uname -s)"; exit 1;;
    esac
    echo "[INFO] Platform: $PLATFORM"
}

# --- Detect Qt6 path (multi-strategy: env var -> PATH -> common dirs) ---
detect_qt() {
    # Strategy 0: QTDIR already set
    if [ -n "${QTDIR:-}" ] && [ -d "$QTDIR/lib/cmake/Qt6" ]; then
        echo "[INFO] Qt6 (QTDIR): $QTDIR"
        return 0
    fi

    # Strategy 1: CMAKE_PREFIX_PATH
    if [ -n "${CMAKE_PREFIX_PATH:-}" ] && [ -d "$CMAKE_PREFIX_PATH/lib/cmake/Qt6" ]; then
        QTDIR="$CMAKE_PREFIX_PATH"
        echo "[INFO] Qt6 (CMAKE_PREFIX_PATH): $QTDIR"
        return 0
    fi

    # Strategy 2: Find qmake in PATH and deduce QTDIR
    if command -v qmake &>/dev/null; then
        local qmake_path
        qmake_path="$(command -v qmake)"
        local qt_candidate
        qt_candidate="$(cd "$(dirname "$qmake_path")/.." && pwd)"
        if [ -d "$qt_candidate/lib/cmake/Qt6" ]; then
            QTDIR="$qt_candidate"
            echo "[INFO] Qt6 (qmake): $QTDIR"
            return 0
        fi
    fi

    # Strategy 3: Search common install paths
    local search_dirs=()
    if [ "$PLATFORM" = "linux" ]; then
        search_dirs=(
            /opt/qt6
            /usr/lib/qt6
            /usr/local/qt6
            "$HOME/Qt"
            "$HOME/Qt6"
        )
    elif [ "$PLATFORM" = "macos" ]; then
        search_dirs=(
            /usr/local/opt/qt@6
            /opt/homebrew/opt/qt@6
            "$HOME/Qt"
            "$HOME/Qt6"
            /usr/local/Cellar/qt
        )
    fi

    for dir in "${search_dirs[@]}"; do
        if [ -d "$dir" ]; then
            # Direct match (e.g., /opt/qt6/6.10.3/gcc_64)
            for ver_dir in "$dir"/6.*; do
                if [ -d "$ver_dir" ]; then
                    for kit_dir in "$ver_dir"/gcc_64 "$ver_dir"/clang_64 "$ver_dir"/macos; do
                        if [ -d "$kit_dir/lib/cmake/Qt6" ]; then
                            QTDIR="$kit_dir"
                            echo "[INFO] Qt6 (search): $QTDIR"
                            return 0
                        fi
                    done
                fi
            done
            # Nested match (e.g., $HOME/Qt/6.10.3/gcc_64)
            if [ -d "$dir/lib/cmake/Qt6" ]; then
                QTDIR="$dir"
                echo "[INFO] Qt6 (search): $QTDIR"
                return 0
            fi
        fi
    done

    echo "[ERROR] Qt6 not found."
    echo "        Set QTDIR to your Qt6 install dir, e.g.:"
    echo "          export QTDIR=/opt/qt6/6.10.3/gcc_64"
    echo "          export QTDIR=\$HOME/Qt/6.10.3/macos"
    return 1
}

# --- Select CMake preset based on platform ---
select_preset() {
    local build_type="${1:-debug}"

    # Prefer CMakeUserPresets.json if it exists
    if [ -f "$PROJECT_ROOT/CMakeUserPresets.json" ]; then
        if [ "$build_type" = "release" ]; then
            CMAKE_PRESET="Qt-Release"
        else
            CMAKE_PRESET="Qt-Debug"
        fi
    else
        if [ "$PLATFORM" = "linux" ]; then
            CMAKE_PRESET="linux-gcc-${build_type}"
        elif [ "$PLATFORM" = "macos" ]; then
            CMAKE_PRESET="macos-clang-${build_type}"
        fi
    fi
    echo "[INFO] CMake preset: $CMAKE_PRESET"
}

# --- Get build output directory from preset ---
get_build_dir() {
    local build_type="${1:-debug}"
    case "$CMAKE_PRESET" in
        linux-gcc-debug)       echo "out/build/linux-debug";;
        linux-gcc-release)     echo "out/build/linux-release";;
        macos-clang-debug)     echo "out/build/macos-debug";;
        macos-clang-release)   echo "out/build/macos-release";;
        Qt-Debug)              echo "out/build/debug";;
        Qt-Release)            echo "out/build/release";;
        *)                     echo "out/build/${build_type}";;
    esac
}

# --- Check prerequisites ---
check_prerequisites() {
    if ! command -v cmake &>/dev/null; then
        echo "[ERROR] cmake not found. Install CMake >= 3.25."
        exit 1
    fi

    local cmake_version
    cmake_version="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+')"
    local cmake_major cmake_minor
    cmake_major="$(echo "$cmake_version" | cut -d. -f1)"
    cmake_minor="$(echo "$cmake_version" | cut -d. -f2)"
    if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 25 ]; }; then
        echo "[ERROR] CMake >= 3.25 required (found: $cmake_version)"
        exit 1
    fi

    if ! command -v ninja &>/dev/null; then
        echo "[WARN] ninja not found, falling back to default generator"
    fi
}
