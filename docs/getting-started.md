# MiniLang 快速开始

本文档涵盖 MiniLang IDE 的构建、运行与开发环境配置的详细说明。

## 环境要求

| 依赖 | 最低版本 | 说明 |
|------|---------|------|
| CMake | 3.25 | 构建系统 |
| C++20 编译器 | MSVC 19.51+ / GCC 13+ / Clang 16+ | 需完整 C++20 支持 |
| Qt6 | 6.0+ | CI 验证版本 6.8.3；需 Core/Gui/Widgets/Svg/Xml |
| Ninja | 任意 | CMake 默认生成器（CMakePresets 指定） |
| GoogleTest | — | 随 `third_party/` 提供，CMake 自动拉取 |

## Windows 构建

### 一键构建（推荐）

项目提供 `configure.bat` 自动检测本机 Visual Studio 与 Qt6 安装位置：

```powershell
# 1. 配置（首次）—— 自动初始化 MSVC 环境 + 检测 Qt6
configure.bat              # Debug（默认）
configure.bat release      # Release

# 2. 编译
cmake --build out/build/debug

# 3. 运行 IDE（Qt DLL 已由 windeployqt 自动部署到同目录）
./out/build/debug/minilang_ide.exe

# 4. 运行单元测试
./out/build/debug/tests/minilang_tests.exe
```

> **Qt 路径**：`configure.bat` 默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`。如安装在别处，先设置环境变量：
> `set QTDIR=<你的Qt路径>/msvc2022_64` 再运行 `configure.bat`。

### 快捷脚本

```powershell
./scripts/build.bat       # 构建 IDE
./scripts/run_tests.bat   # 构建并运行测试
```

### 手动配置

```powershell
# 方式 A：使用仓库内 CMake Preset
cmake --preset windows-msvc-debug
cmake --build out/build/debug

# 方式 B：手动指定生成器与 Qt 路径
cmake -S . -B out/build/debug -G "Ninja" `
  -DCMAKE_PREFIX_PATH="<Qt路径>\msvc2022_64"
cmake --build out/build/debug
```

## Linux / macOS 构建

### 使用构建脚本（推荐）

```bash
# 自动检测 Qt6、配置并构建
./scripts/build.sh            # Debug（默认）
./scripts/build.sh release    # Release

# 仅配置
./scripts/configure.sh
./scripts/configure.sh release

# 构建并运行测试
./scripts/run_tests.sh
./scripts/run_tests.sh release

# 运行已构建的测试（不重新编译）
./scripts/run_tests_only.sh
```

### 手动构建

```bash
# 设置 Qt6 路径（如 QTDIR 未设置）
export QTDIR=/opt/qt6/6.8.3/gcc_64    # Linux
export QTDIR=~/Qt/6.8.3/macos          # macOS

# 使用 CMake Preset
cmake --preset linux-gcc-release         # Linux
cmake --preset macos-clang-release       # macOS

# 构建
cmake --build out/build/linux-release    # Linux
cmake --build out/build/macos-release    # macOS

# 运行
./out/build/linux-release/minilang_ide   # Linux
./out/build/macos-release/minilang_ide   # macOS
```

### Qt6 安装

**Ubuntu/Debian**：
```bash
# 方式 1：系统包（版本可能较旧）
sudo apt install qt6-base-dev libqt6svg6-dev

# 方式 2：aqtinstall（推荐，可指定精确版本）
pip3 install aqtinstall
python3 -m aqt install-qt linux desktop 6.8.3 gcc_64 -m qtsvg -O /opt/qt6
export QTDIR=/opt/qt6/6.8.3/gcc_64
```

**macOS (Homebrew)**：
```bash
brew install qt@6
export QTDIR=$(brew --prefix qt@6)
```

## Docker 构建

项目提供多阶段 Dockerfile（Ubuntu 24.04 + Qt 6.8.3），与 CI 版本保持一致。

```bash
# 构建并运行（无 GUI）
docker compose run --rm build

# 开发模式（含 X11 GUI 转发）
# Linux: 先执行 xhost +local:
docker compose up dev

# 手动构建
docker build -t minilang .
docker run --rm minilang
```

> **X11 转发**：Linux 需 `xhost +local:`；macOS 需安装 XQuartz 并设置 `DISPLAY=host.docker.internal:0`；Windows 需安装 VcXsrv。

## 运行 IDE

构建完成后运行 IDE：

```text
# Windows
./out/build/debug/minilang_ide.exe

# Linux
./out/build/linux-release/minilang_ide

# macOS
./out/build/macos-release/minilang_ide
```

在编辑器中输入以下代码，点击运行：

```text
print("Hello, MiniLang!");

fun fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

for (var i = 0; i < 10; i = i + 1) {
    print(fib(i));
}
```

## 开发环境

### 推荐 IDE

- **CLion**：直接打开 `CMakeLists.txt`，CMake Preset 自动识别，调试体验最佳
- **Qt Creator**：打开 `CMakeLists.txt`，Qt 集成度高
- **VS Code**：安装 CMake Tools + C/C++ 扩展，轻量灵活

### CMake Preset 说明

项目采用双层 Preset 体系：

- `CMakePresets.json`（仓库内）：共享配置，定义 `windows-msvc-debug` / `linux-gcc-release` / `macos-clang-debug` 等通用 Preset
- `CMakeUserPresets.json`（gitignore）：机器特定配置，存放 Qt 安装路径等本地信息

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `MINILANG_UNITY_BUILD` | OFF | Unity Build 批量编译，首次构建加速 30-50% |
| `MINILANG_VM_PROFILING` | OFF | VM opcode 执行计数 profiling |
| `MINILANG_BUILD_DOCS` | OFF | Doxygen API 文档生成 |
| `MINILANG_ENABLE_I18N` | ON | Qt Linguist 国际化 i18n 工具链（默认编译 .ts → .qm 并部署到 exe 旁） |
| `MINILANG_ENABLE_LTO` | ON | Release 构建启用链接时优化 |
| `MINILANG_USE_CCACHE` | OFF | 启用 ccache 缓存 |
| `MINILANG_W4` | OFF | MSVC /W4 警告级别 + 释放 /wd4996（windows-msvc-debug/release preset 默认 ON） |
| `MINILANG_WERROR` | OFF | 将编译器警告视为错误（所有平台 debug/release preset 默认 ON：MSVC /WX、GCC/Clang -Werror） |
| `MINILANG_BUILD_PERF_TESTS` | ON | 构建性能基准测试目标 minilang_perf_test |
| `MINILANG_BUILD_TESTS` | ON | 构建 GoogleTest 单元测试 |
| `MINILANG_BUILD_TEST_HARNESS` | OFF | 构建审计/压测工具 |
| `MINILANG_USE_QTCHARTS` | OFF | 使用 QtCharts 绘制柱状图（性能仪表盘等） |
| `MINILANG_USE_JIT` | ON (x86-64) / OFF (其他) | JIT 后端（asmjit），仅 x86-64 平台启用；非 x86-64 自动禁用回退到 StackVM |
| `MINILANG_USE_JIT_A64` | OFF | **实验性** ARM64 JIT 后端（PoC，仅基本算术+控制流，无测试覆盖，不建议生产使用） |
| `MINILANG_SANITIZE` | `""` | Sanitizer：`address` / `undefined` / `both` |

```bash
# 启用 VM profiling + Doxygen 文档
cmake --preset windows-msvc-debug -DMINILANG_VM_PROFILING=ON -DMINILANG_BUILD_DOCS=ON
cmake --build out/build/debug --target minilang_tests docs
```

## 测试

```powershell
# 方式 1：直接运行测试二进制
./out/build/debug/tests/minilang_tests.exe

# 方式 2：通过 CTest（可筛选）
cd out/build/debug
ctest                                  # 运行全部
ctest -R LexerTest.* --verbose         # 筛选指定套件
ctest -R "VME2E\..*" --output-on-failure
```

## 调试技巧

- **解释器调试**：在 IDE 中点击“调试”按钮，支持断点、单步、变量监视、调用栈
- **VM 调试**：打开“VM 栈面板”查看字节码执行过程，支持单步执行
- **IR 调试**：启用 IR 路径后打开“IR 可视化面板”，查看中间表示与优化效果
- **单元测试调试**：在 CLion/Qt Creator 中直接运行 GoogleTest 用例

## CLI 工具链

项目提供 9 个独立命令行工具，构建后位于对应构建目录下：

| 工具 | 用途 | 示例 |
|------|------|------|
| `minilang-fmt` | 代码格式化 | `minilang-fmt --check src.ml` |
| `minilang-lint` | 静态分析 (8 项检查规则) | `minilang-lint src.ml --format json` |
| `minilang-coverage` | 行级覆盖率 (text/LCOV) | `minilang-coverage --backend both src.ml` |
| `minilang-doc` | API 文档生成 (Markdown/HTML/JSON) | `minilang-doc src.ml --format html` |
| `minilang-fuzz` | 三后端差分模糊测试 | `minilang-fuzz --mode mutate --seed 42` |
| `minilang-lsp` | Language Server Protocol 实现 | 编辑器 stdio 连接 |
| `minilang-dap` | Debug Adapter Protocol 实现 | VS Code launch.json 配置 |
| `minilang-compile` | 预编译模块 (.minic) | `minilang-compile --module lib.ml` |
| `minilang-pkg` | 包管理器 | `minilang-pkg install` |

所有工具支持 `--help` 查看完整参数说明。退出码语义：0=成功、1=有警告、2=错误。

## 构建问题排查

- **Windows SEH 0xc0000005 崩溃**（修改头文件 struct 布局后）：删除 `out/build/debug` 目录后全量重配置 + 重编译
- **Qt 找不到**：确认 `QTDIR` 环境变量指向 Qt 安装前缀（含 `lib/cmake/Qt6` 子目录）
- **Ninja 找不到**：安装 Ninja 或使用 `-G "Unix Makefiles"` 替代

详细工程约定参见 [development.md](development.md)。
