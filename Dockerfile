# ============================================================
# MiniLang IDE 开发环境 Docker
# 基于 Ubuntu 24.04（apt 提供 Qt 6.4.2）
# 用法: docker compose up dev   # 进入开发 shell（支持 GUI）
#        docker compose build    # 构建项目
# ============================================================

FROM ubuntu:24.04 AS base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y     build-essential cmake ninja-build git gdb     libgl1-mesa-dev libegl1-mesa-dev     libxkbcommon-x11-dev     ca-certificates locales     && locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LANGUAGE=en_US.UTF-8 LC_ALL=en_US.UTF-8

# Install Qt 6（Ubuntu 24.04 apt 提供 Qt 6.4.2）
RUN apt-get install -y qt6-base-dev qt6-svg-dev qt6-tools-dev     libqt6charts6-dev     || (echo "Qt6 apt install failed" && exit 1)

# 设置 QTDIR 供 CMake preset 使用
ENV QTDIR=/usr/lib/x86_64-linux-gnu/qt6

WORKDIR /src

# ---- builder: Release 构建 ----
FROM base AS builder
COPY . .
# 使用 CMakePresets 的 linux-gcc-release 配置，与本地构建保持一致
RUN cmake --preset linux-gcc-release     && cmake --build out/build/linux-release --parallel

# ---- dev: 交互式开发环境（支持 Qt GUI）----
FROM base AS dev
# 安装 X11 依赖用于 GUI 转发
RUN apt-get install -y libx11-6 libxext6 libxrender1 libxcb-xinerama0     libxcb-cursor0 libfontconfig1 x11-apps 2>/dev/null || true
WORKDIR /src
CMD ["bash"]
