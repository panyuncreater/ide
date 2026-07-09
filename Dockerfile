# ============================================================
# MiniLang IDE Dockerfile
# ------------------------------------------------------------
# 多阶段构建：base（共用基础/Qt 运行时）→ builder（编译）→ dev（运行/开发）
#
# 关键说明：
#   1. Qt 版本固定为 6.8.3 LTS，与 CI（.github/workflows/ci.yml 的 QT_VERSION）
#      保持一致。Ubuntu 24.04 apt 仅提供 Qt 6.4.2，若代码使用了 6.5+ 引入的
#      API，apt 版本会导致编译失败，因此改用 aqtinstall 安装精确版本。
#      Qt 6.7+ 在 Linux 上将架构名从 gcc_64 改为 linux_gcc_64，aqtinstall
#      ≤3.2.1 仍用旧名导致 "qt_base not found"（issue #908），3.3.0 修复。
#      qttools 模块在 Qt 6.8.3 元数据中不可用，故不安装。
#      可通过 docker build --build-arg QT_VERSION=6.x.y 覆盖。
#   2. QTDIR 直接指向 Qt 安装前缀 /opt/qt6/<version>/linux_gcc_64（内含
#      lib/cmake/Qt6），供 CMakePresets 的 linux-gcc-release 预设通过
#      $env{QTDIR} 正确定位 Qt6（原 /usr/lib/x86_64-linux-gnu/qt6 前缀错误）。
# ============================================================

# ---- 基础镜像（构建/运行共用）----
FROM ubuntu:24.04 AS base
ARG QT_VERSION=6.8.3
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    ccache \
    pkg-config \
    libgl1 \
    libglib2.0-0 \
    libxkbcommon0 \
    libdbus-1-3 \
    libfontconfig1 \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

# 安装指定版本 Qt（通过 aqtinstall，与 CI 同步）
# 使用 venv 安装 aqtinstall，绕过 Ubuntu 24.04 PEP 668 限制
# aqtinstall 3.3.0：修复 Qt 6.7+ Linux 架构名推导（gcc_64 → linux_gcc_64，issue #908）
# qttools/qtsvg 模块在 Qt 6.8.3 的 aqtinstall 元数据中不可用，故仅安装 base
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir aqtinstall==3.3.0 \
    && /opt/venv/bin/python -m aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 -O /opt/qt6
ENV PATH="/opt/venv/bin:${PATH}"
ENV QTDIR=/opt/qt6/${QT_VERSION}/linux_gcc_64
ENV PATH=${QTDIR}/bin:${PATH}
ENV LD_LIBRARY_PATH=
ENV LD_LIBRARY_PATH="${QTDIR}/lib:${LD_LIBRARY_PATH}"
ENV QT_QPA_PLATFORM_PLUGIN_PATH=${QTDIR}/plugins

# ---- 构建阶段 ----
FROM base AS builder
WORKDIR /app

# 第一层：第三方依赖（体积大、变化频率低，单独缓存，命中率最高）
COPY third_party/ third_party/

# 第二层：CMake 配置脚本（变化频率低）
COPY CMakeLists.txt CMakePresets.json cmake/ ./

# 仅配置 IDE：关闭测试与 i18n，避免 configure 阶段依赖源码目录，
# 从而让源码层的变化不会使本层缓存失效（修复审计问题 5）。
RUN cmake --preset linux-gcc-release \
    -DMINILANG_BUILD_TESTS=OFF \
    -DMINILANG_ENABLE_I18N=OFF

# 第三层：源代码（变化频率高）
COPY app/ app/
COPY common/ common/
COPY lexer/ lexer/
COPY parser/ parser/
COPY ast/ ast/
COPY interpreter/ interpreter/
COPY compiler/ compiler/
COPY debug/ debug/
COPY formatter/ formatter/
COPY gui/ gui/
COPY tests/ tests/

# 再构建（仅受源码层变化影响，复用上方 configure 缓存）
RUN cmake --build out/build/linux-release --parallel

# ---- 开发/运行阶段 ----
FROM base AS dev
WORKDIR /app
COPY --from=builder /app/out/build/linux-release/minilang_ide .
CMD ["/bin/bash"]
