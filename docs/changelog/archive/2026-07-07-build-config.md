# MiniLang 变更日志归档

本文件包含 MiniLang IDE 早期开发阶段的工程配置与文档一致性修复记录（2026-07-07）。

---

## 2026-07-07 · 工程配置与文档一致性修复（build）

### 概述

修复构建系统配置与文档之间的一系列不一致，提升可复现性与新贡献者上手体验：

- **CMake 最低版本**：`cmake_minimum_required` 由 3.20 提升至 **3.25**，与 `CMakePresets.json` 的 schema v6 要求对齐（CI 与各预设此前已隐含需要 3.25）。
- **子模块可复现性**：移除 `.gitmodules` 中 googletest / ads / QFluentKit 的 `branch =` 字段，避免 `git submodule update --remote` 偏离已锁定的 gitlink 提交。
- **Docker 构建修复**：`Dockerfile` 改用 aqtinstall 安装经验证版本 **Qt 6.10.3**（与 CI `QT_VERSION` 一致），并将 `QTDIR` 指向正确的 Qt 安装前缀（`/opt/qt6/6.10.3/gcc_64`），修复此前因 `QTDIR=/usr/lib/x86_64-linux-gnu/qt6` 前缀错误导致 `find_package(Qt6)` 失败的问题；同时移除未启用的 `libqt6charts6-dev`。
- **CI 跨平台一致性**：Unix `build-test` 作业直连 cmake 增加 `-DMINILANG_WERROR=ON -DMINILANG_ENABLE_LTO=ON`，与 Windows 预设的告警/优化策略对齐。
- **代码规范**：`.clang-tidy` 的 `modernize-use-nullptr.NullPointerLiteral` 由 `'0'` 修正为 `'nullptr'`；清理 `.gitattributes` 中对已被 gitignore 的 `CMakeUserPresets.json` 的冗余 eol 声明。
- **文档同步**（详见 `README.md` / `CONTRIBUTING.md`）：更正 Windows 构建命令的 Preset 名称与输出路径（`windows-msvc-debug` → `out/build/debug` 等）、CMake 最低版本、Qt 经验证版本、编译器最低版本；补全 CMake 选项说明表；修正 `configure.bat` 行为描述（不会自动生成 `CMakeUserPresets.json`）；测试中记录的单元测试数量统一为 1699（经程序化复核，源码 `TEST*`/`TYPED_TEST*` 宏确为 1699 处；无参数化/禁用用例）。

### 验证结果

- MSVC 19.51 + Qt 6.10.3 + Ninja 配置修复通过；Docker/CI 配置修复需相应流水线实测验证。
- 源码中 `TEST*`/`TYPED_TEST*` 宏共 **1699** 处（与文档记录一致；无参数化/禁用用例）。
