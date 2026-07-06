# MiniLang IDE 优化路线图

> 本文档由项目审计自动生成，供 AI Agent 逐项执行。
> 审计基准：2026-07-05，代码总量 ~74,122 行（核心 50,648 + 测试 23,434 + 构建 140）。

---

## 目录

- [一、高优先级（本周完成）](#一高优先级本周完成)
  - [1.1 CI 跨平台支持](#11-ci-跨平台支持)
  - [1.2 CMakePresets.json 补充非 Windows 配置](#12-cmakepresetsjson-补充非-windows-配置)
  - [1.3 翻译文件填充](#13-翻译文件填充)
  - [1.4 README 截图补充](#14-readme-截图补充)
- [二、中优先级（下周完成）](#二中优先级下周完成)
  - [2.1 示例程序扩充](#21-示例程序扩充)
  - [2.2 测试命名统一](#22-测试命名统一)
  - [2.3 覆盖率集成 codecov](#23-覆盖率集成-codecov)
  - [2.4 新建 CONTRIBUTING.md](#24-新建-contributingmd)
  - [2.5 文档拆分重构](#25-文档拆分重构)
- [三、低优先级（月内评估）](#三低优先级月内评估)
  - [3.1 Docker 开发环境](#31-docker-开发环境)
  - [3.2 QFluentKit 依赖解耦评估](#32-qfluentkit-依赖解耦评估)
  - [3.3 scripts/ 去硬编码路径](#33-scripts-去硬编码路径)
  - [3.4 Logo SVG 元数据补充](#34-logo-svg-元数据补充)
  - [3.5 CI 性能基准回归检测](#35-ci-性能基准回归检测)
- [四、遗留 P1 问题修复（上一轮审计发现）](#四遗留-p1-问题修复上一轮审计发现)
  - [4.1 Sanitizer 跨平台补全](#41-sanitizer-跨平台补全)
  - [4.2 根目录 .obj 文件清理](#42-根目录-obj-文件清理)
  - [4.3 CMakeLists.txt CPack 语法错误修复](#43-cmakeliststxt-cpack-语法错误修复)
- [五、QSS 双主题对称性修复清单](#五qss-双主题对称性修复清单)

---

## 一、高优先级（本周完成）

### 1.1 CI 跨平台支持

**文件**: `.github/workflows/ci.yml`

**现状**: 仅 `windows-latest` 一个 runner，Linux/macOS 提交无法在 CI 中验证。

**任务**:

1. 在 `ci.yml` 的 `build-test` job 中添加 strategy matrix：

```yaml
strategy:
  fail-fast: false
  matrix:
    include:
      - os: windows-latest
        arch: win64_msvc2022_64
        generator: Ninja
        c_compiler: cl
        cpp_compiler: cl
      - os: ubuntu-latest
        arch: gcc_64
        generator: Ninja
        c_compiler: gcc
        cpp_compiler: g++
      - os: macos-latest
        arch: clang_64
        generator: Ninja
        c_compiler: clang
        cpp_compiler: clang++
```

2. Qt 安装步骤需要根据 OS 调整：
   - Windows: 使用 `jurplel/install-qt-action@v4`
   - Ubuntu: 通过 apt 或 installer
   - macOS: 通过 brew 或 installer

3. 确保 `coverage` 和 `static-analysis` job 也仅在 Linux 上运行（gcov 最成熟）

**验收标准**: PR 到 main 时同时触发 3 个平台的构建+测试。

---

### 1.2 CMakePresets.json 补充非 Windows 配置

**文件**: `CMakePresets.json`

**现状**: 5 个 configure presets 全部带 `"condition": { "lhs": "${hostSystemName}", "rhs": "Windows" }`

**任务**: 添加以下 preset：

```json
{
  "name": "linux-gcc-debug",
  "displayName": "Linux GCC Debug (Ninja)",
  "description": "GCC + Ninja Debug 构建",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/out/build/debug",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug"
  },
  "condition": {
    "type": "equals",
    "lhs": "${hostSystemName}",
    "rhs": "Linux"
  }
},
{
  "name": "linux-gcc-release",
  "displayName": "Linux GCC Release (Ninja)",
  "description": "GCC + Ninja Release 构建（含 LTO）",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/out/build/release",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Release",
    "MINILANG_WERROR": "ON",
    "MINILANG_ENABLE_LTO": "ON"
  },
  "condition": {
    "type": "equals",
    "lhs": "${hostSystemName}",
    "rhs": "Linux"
  }
},
{
  "name": "macos-clang-debug",
  "displayName": "macOS Clang Debug (Ninja)",
  "description": "Clang + Ninja Debug 构建",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/out/build/debug",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug"
  },
  "condition": {
    "type": "equals",
    "lhs": "${hostSystemName}",
    "rhs": "Darwin"
  }
}
```

同步添加对应的 testPresets。

**验收标准**: Linux/macOS 用户可使用 `cmake --preset linux-gcc-debug` 配置。

---

### 1.3 翻译文件填充

**文件**: `app/translations/minilang_zh_CN.ts`

**现状**: 仅 2 条 `type="unfinished"` 占位条目。

**前置条件**: 需要先审计源码中所有用户可见字符串是否已用 `mlTr()`/`tr()` 包裹。

**任务**:

#### Step 1: 审计源码中的 UI 字符串

搜索以下目录中所有可见字符串：
- `app/*.cpp`, `app/*.h` — IDE 主程序
- `gui/*.cpp`, `gui/*.h` — GUI 面板

重点关注：
- `QMenu::addAction("...")` 中的菜单文本
- `QLabel/QPushButton` 构造时的显示文本
- `QMessageBox::information/warning/critical` 的标题和内容
- `setStatusTip/setToolTip` 的提示文字
- `main.cpp` 中的启动错误消息

#### Step 2: 确保字符串已包裹

未包裹的字符串改为：
```cpp
// 修改前
menu->addAction("运行");

// 修改后
menu->addAction(tr("运行"));
// 或使用项目封装
menu->addAction(mlTr("运行"));
```

#### Step 3: 填充翻译文件

将 `minilang_zh_CN.ts` 中的 `<translation type="unfinished">` 替换为实际中文翻译。最低限度覆盖以下类别（约 60-80 条）：

| 类别 | 示例原文 | 中文翻译 |
|---|---|---|
| 文件菜单 | &File, &New, &Open, &Save, Save &As, E&xit | 文件(&F), 新建(&N), 打开(&O), 保存(&S), 另存为(&A), 退出(&X) |
| 编辑菜单 | &Edit, &Undo, &Redo, Cu&t, &Copy, &Paste | 编辑(&E), 撤销(&U), 重做(&R), 剪切(&T), 复制(&C), 粘贴(&P) |
| 视图菜单 | &View, &Theme, &Dark Mode, &Light Mode | 视图(&V), 主题(&T), 暗色模式(&D), 亮色模式(&L) |
| 运行菜单 | &Run, Run &Code, &Stop, &Debug | 运行(&R), 运行代码(&C), 停止(&S), 调试(&D) |
| 工具栏按钮 | Run, Stop, Step Into, Step Over, Step Out | 运行, 停止, 单步进入, 单步跳过, 单步跳出 |
| 面板标题 | Output, Error List, AST Viewer, Bytecode, Variables, Call Stack | 输出, 错误列表, AST 视图, 字节码, 变量, 调用栈 |
| 对话框 | Open Folder, New File, Save As | 打开文件夹, 新建文件, 另存为 |
| 错误消息 | MiniLang IDE - Startup Error, Unknown startup exception | MiniLang IDE - 启动错误, 未知的启动异常 |
| 状态栏 | Ready, Running, Debugging | 就绪, 运行中, 调试中 |

**验收标准**: 启用 `-DMINILANG_ENABLE_I18N=ON` 构建后，IDE 界面主要元素显示中文。

---

### 1.4 README 截图补充

**文件**: `README.md`

**现状**: 写着 `> 截图待补充`。

**任务**:

1. 构建并运行 IDE（亮色和暗色主题各一次）
2. 截取以下场景截图，放入 `docs/screenshots/` 目录：
   - `main-light.png` — IDE 主界面（编辑器 + 侧栏面板），亮色主题
   - `main-dark.png` — 同上，暗色主题
   - `debugging.png` — 调试状态（断点高亮 + 变量面板 + 调用栈）
   - `ast-bytecode.png` — AST 可视化 + 字节码面板
3. 更新 README.md 中的截图区域：

```markdown
## 界面预览

### 主界面（亮色主题）
![主界面亮色](docs/screenshots/main-light.png)

### 主界面（暗色主题）
![主界面暗色](docs/screenshots/main-dark.png)

### 调试模式
![调试模式](docs/screenshots/debugging.png)

### AST 与字节码可视化
![AST与字节码](docs/screenshots/ast-bytecode.png)
```

**验收标准**: README 渲染后可看到 4 张截图预览。

---

## 二、中优先级（下周完成）

### 2.1 示例程序扩充

**目录**: `samples/mini/`

**现状**: 仅 4 个文件共 131 行。

**任务**: 创建以下示例文件（每个需覆盖指定语言特性，附带中文注释）：

#### `hello.mini` (~8 行)
```mini
// 基础输出与变量
fun main() {
    print("Hello, MiniLang!");
    var name = "World";
    print("Hello, {name}!");
}
```
覆盖：print、字符串插值、var 声明

#### `fibonacci.mini` (~20 行)
覆盖：函数定义、递归、int 类型、闭包捕获

#### `oop_shapes.mini` (~45 行)
覆盖：class / extends / super / 方法调用 / this 绑定

#### `collections.mini` (~35 行)
覆盖：数组 [] / 字典 {} / 内置方法 push/pop/len/has/COW 语义

#### `error_handling.mini` (~30 行)
覆盖：try / catch / throw / 异常传播

#### `modules_demo.mini` + 依赖模块 (~60 行，3 个文件)
覆盖：import / export / 模块隔离 / 路径安全

#### `string_interp.mini` (~20 行)
覆盖：字符串插值嵌套、表达式嵌入、类型转换

#### `higher_order.mini` (~25 行)
覆盖：一等函数、回调、闭包 upvalue

#### `type_system.mini` (~25 行)
覆盖：类型注解强制、int48 边界、float 宽化、null 兼容

#### `control_flow.mini` (~25 行)
覆盖：if/else、while、for、break/continue、and/or 短路

**每个示例的要求**:
- 文件头注释说明覆盖的语言特性
- 关键行有中文注释解释语义
- 代码可直接运行（不依赖其他文件，除非演示模块系统）
- 最后附上预期输出（以注释形式）

**验收标准**: `samples/mini/` 下有 13-15 个 `.mini` 文件，总计 300+ 行。

---

### 2.2 测试命名统一

**目录**: `tests/`

**现状**: 25 个测试文件命名风格不一致。

**重命名映射表**:

| 当前名称 | 建议新名称 | 说明 |
|---|---|---|
| `TestCompilerAudit.cpp` | `TestCompilerAudit.cpp` | ✅ 保持不变 |
| `TestCompilerAudit2.cpp` | `TestCompilerIRIsolationAudit.cpp` | 描述性命名 |
| `TestTeachingPanelsAudit.cpp` | `TestTeachingPanelsAudit1.cpp` | ✅ 保持不变（第一波） |
| `TestTeachingPanelsAudit2.cpp` | `TestTeachingPanelsAudit2.cpp` | ✅ 保持不变（第二波） |
| `TestTeachingPanelsAudit3.cpp` | `TestTeachingPanelsAudit3.cpp` | ✅ 保持不变（第三波） |
| `TestTeachingPanelsAudit4.cpp` | `TestTeachingPanelsAudit4.cpp` | ✅ 保持不变（第二档+第四档） |
| `TestTeachingPanelsAudit5.cpp` | `TestTeachingPanelsAudit5.cpp` | ✅ 保持不变（第三档） |
| `TestConsistencyDiff.cpp` | `TestThreeEnginesConsistency.cpp` | 三后端一致性 |
| `TestMethodCallProbe.cpp` | `TestMethodCallProbe.cpp` | ✅ 保持不变 |
| `TestNestedLvalueProbe.cpp` | `TestNestedLvalueProbe.cpp` | ✅ 保持不变 |
| `TestFormatterGUISuiteAudit.cpp` | `TestFormatterAudit.cpp` | 统一审计命名 |

**注意**: 
- 重命名时必须同步更新 `tests/CMakeLists.txt` 中的源文件列表
- 确保所有 TEST_F / TEST_P 宏的套件名不受影响（套件名在文件内容中，不在文件名）
- git mv 而非直接重写，保留历史

**建议的未来约定**:
```
Test{Module}.cpp              # 基础功能测试
Test{Module}Audit.cpp         # 审计/回归测试
Test{Module}E2E.cpp           # 端到端集成测试
Test{Module}{Specific}.cpp    # 特定专项测试（如 Probe）
```

**验收标准**: 所有测试文件遵循一致的命名约定，CMakeLists.txt 引用正确，`ctest` 全量通过。

---

### 2.3 覆盖率集成 codecov

**文件**: `.github/workflows/ci.yml`

**现状**: CI 有 coverage job 但未上传到外部服务。

**任务**:

1. 在 CI workflow 中添加 coverage 生成步骤（仅 Linux）：

```yaml
coverage:
  name: Code Coverage
  runs-on: ubuntu-latest
  needs: build-test
  if: github.event_name == 'push' && github.ref == 'refs/heads/main'
  steps:
    - uses: actions/checkout@v4
      with:
        submodules: recursive
    - uses: jurplel/install-qt-action@v4
      with:
        version: ${{ env.QT_VERSION }}
        host: linux
        target: desktop
        arch: gcc_64
        cache: true
    - name: Configure with Coverage
      run: cmake -B build -DCMAKE_BUILD_TYPE=Debug
            -DCMAKE_CXX_FLAGS="--coverage -fprofile-arcs -ftest-coverage"
            -DCMAKE_EXE_LINKER_FLAGS="--coverage -fprofile-arcs -ftest-coverage"
    - name: Build for Coverage
      run: cmake --build build
    - name: Run Tests
      working-directory: build
      run: ctest --output-on-failure -j4
    - name: Generate Coverage
      working-directory: build
      run: gcovr -r ../ --xml -o coverage.xml --filter '.*app/.*|.*gui/.*|.*compiler/.*|.*interpreter/.*|.*lexer/.*|.*parser/.*'
    - name: Upload to Codecov
      uses: codecov/codecov-action@v4
      with:
        files: ./build/coverage.xml
        flags: unittests
        name: minilang-coverage
        fail_ci_if_error: false
```

2. （可选）在 README.md 中添加覆盖率 badge：
```markdown
![Coverage](https://img.shields.io/codecov/c/github/yourrepo/minilang ide)
```

**验收标准**: push 到 main 后自动生成覆盖率报告并上传到 codecov.io。

---

### 2.4 新建 CONTRIBUTING.md

**文件**: `CONTRIBUTING.md`（新建）

**任务**: 创建以下结构的贡献指南：

```markdown
# Contributing to MiniLang IDE

感谢你对 MiniLang IDE 的贡献！

## 开发环境要求

### 必需
- C++20 兼容编译器（MSVC 19.30+ / GCC 11+ / Clang 14+）
- Qt 6.0+（Core, Gui, Widgets, Svg, Xml）
- CMake 3.20+
- Ninja（推荐）或 Make

### 推荐
- Visual Studio 2022（Windows）或 CLion / VS Code
- ccache（加速增量构建）

## 快速搭建

### Windows
[引用 scripts/build_ide.bat 的步骤]

### Linux
```bash
sudo apt install qt6-base-dev qt6-svg-dev cmake ninja-build g++ cmake
git clone <repo-url>
cd minilang-ide
cmake -B out/build/debug -GNinja -DCMAKE_BUILD_TYPE=Debug \
      -DQT_DIR=/path/to/qt6
cmake --build out/build/debug
./out/build/debug/minilang_ide
```

### macOS
[类似步骤]

## 编码规范

项目已配置自动化代码风格检查工具：

- **格式化**: `.clang-format`（LLVM 风格，4 空格缩进，120 列限制）
  ```bash
  # 格式化所有源文件
  find . -name "*.cpp" -o -name "*.h" | xargs clang-format -i
  ```
- **静态分析**: `.clang-tidy`（bugprone/modernize/performance/readability 规则集）
  ```bash
  # 运行 tidy 检查
  cmake --build out/build/debug --target tidy
  ```

### 代码风格要点
- 4 空格缩进，不用 Tab
- 类名 CamelCase，方法/变量 camelBack，成员变量加 `_` 后缀
- 花括号不换行（Attach 风格）
- 列宽限制 120 字符
- 指针左对齐：`int* ptr` 而非 `int *ptr`

## 提交信息格式

参考 CHANGELOG 中的结构化格式：

```
<类型>(<范围>): <简短描述>

<详细说明（可选）>

- 具体变更点 1
- 具体变更点 2

Fixes #<issue编号>
```

类型：feat / fix / docs / style / refactor / test / chore / perf / ci

范围：compiler / interpreter / gui / lexer / parser / ide / build / test / docs

## PR 流程

1. Fork 并创建特性分支（`git checkout -b feat/your-feature`）
2. 确保代码通过格式化和静态分析
3. 确保全量测试通过（1300+ 测试无回归）
4. **三后端一致性验证**：涉及语言语义的改动必须在 Interpreter / StackVM / RegisterVM 上均通过
5. 提交 PR，填写模板描述变更内容
6. 等 CI 全绿后合并

## 三后端一致性验证（重要！）

MiniLang 的核心约束是同一源码在三条执行路径上产生相同结果。如果你修改了以下任一模块，**必须**验证三后端行为一致：

- `interpreter/` — 树遍历解释器
- `compiler/VM.cpp` — 栈式 VM
- `compiler/RegisterVM.cpp` — 寄存器式 VM
- `compiler/Compiler.cpp` — 编译器前端（影响所有后端）
- `compiler/IR.cpp` — IR 层（影响 StackVM 和 RegisterVM）

验证方法：运行 `TestThreeEnginesAudit` 和 `TestVME2E` 测试套件。

## 测试指南

### 运行全部测试
```bash
cd out/build/debug
ctest --output-on-failure -j4
```

### 运行特定测试
```bash
./minilang_tests --gtest_filter="TestLexer.*"
./minilang_tests --gtest_filter="TestCompiler*:*"
```

### 添加新测试
- 基础功能测试放入 `Test{模块名}.cpp`
- 回归测试放入 `Test{模块名}Audit.cpp`
- 教学面板数据完整性测试放入 `TestTeachingPanelsAudit{N}.cpp`
- 使用 GoogleTest 的 `TEST_F` 宏编写

## 国际化（i18n）

所有用户可见字符串必须用 `mlTr()` 包裹：
```cpp
// 正确
statusBar()->showMessage(mlTr("Ready"));
// 错误
statusBar()->showMessage("Ready");
```

启用 i18n 构建：
```bash
cmake -B build -DMINILANG_ENABLE_I18N=ON
```

## 需要帮助？

- 查看 `docs/development.md` 了解架构设计决策
- 查看 `AGENTS.md` 了解 AI 审计协议
- 提 Issue 讨论设计问题
```

**验收标准**: 文件存在于项目根目录，内容完整可用。

---

### 2.5 文档拆分重构

**文件**: `docs/development.md`（522 行 → 拆分为多个文件）

**现状**: 超长文档混合了设计决策、实施日志、Bug 修复记录。

**目标结构**:

```
docs/
├── development.md          # 保留，精简为架构概览 + 入口索引（~100 行）
├── architecture.md         # 新建：核心架构设计原则
├── design-decisions/       # 新建目录：ADR 系列
│   ├── ADR-001-nan-boxing.md
│   ├── ADR-002-triple-backend.md
│   ├── ADR-003-ir-layer.md
│   ├── ADR-004-cow-memory.md
│   └── ADR-005-debug-consistency.md
├── testing-guide.md        # 新建：三后端一致性验证方法论
└── changelog/
    └── 2026-07-05.md       # 从 development.md 提取当次更新记录
```

#### `docs/architecture.md` 内容大纲（从 development.md 提炼）：

```markdown
# MiniLang IDE 架构设计

## 执行管线概览
## 内存模型
## 三后端一致性约束
## GUI 线程模型
## 模块依赖关系图（文字描述）
## 设计原则索引（链接到各 ADR）
```

#### 每个 ADR 模板：

```markdown
# ADR-{NNN}: {标题}

## 状态
已采纳 / 已实施 / 已废弃

## 背景
描述问题背景和上下文

## 决策
做了什么选择及其原因

## 影响
对代码/性能/维护性的影响

## 替代方案
考虑过但未采用的其他方案及理由
```

#### 从 development.md 提取的 ADR 列表：

| 编号 | 标题 | 来源章节 |
|---|---|---|
| ADR-001 | NaN-boxing Value 编码 | PERF-12 |
| ADR-002 | 三后端执行引擎并存 | PERF-14 + 核心约束 |
| ADR-003 | IR 中间表示层 | IR 中间层 |
| ADR-004 | Copy-On-Write 语义 | COW 优化 |
| ADR-005 | 调试器状态一致性 | 调试一致性 |

**验收标准**: `docs/` 目录结构清晰，每个文档职责单一，`development.md` 缩减至 ~100 行的导航文档。

---

## 三、低优先级（月内评估）

### 3.1 Docker 开发环境

**文件**: `Dockerfile`（新建）

**目标**: 让新贡献者无需手动安装 Qt6 + CMake + 编译器即可开始开发。

**任务**: 创建多阶段 Dockerfile：

```dockerfile
# ============================================================
# MiniLang IDE 开发环境 Docker
# 用法: docker compose up dev   # 进入开发 shell
#        docker compose build    # 构建项目
# ============================================================

FROM ubuntu:22.04 AS base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build git gdb \
    libgl1-mesa-dev libegl1-mesa-dev \
    libxkbcommon-x11-dev \
    ca-certificates locales \
    && locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LANGUAGE=en_US.UTF-8 LC_ALL=en_US.UTF-8

# 安装 Qt 6（官方 installer 或 ppa）
RUN apt-get install -y qt6-base-dev qt6-svg-dev qt6-tools-dev \
    || (echo "Qt6 apt 安装失败，请使用官方 installer" && exit 1)

WORKDIR /src

FROM base AS builder
COPY . .
RUN cmake -B build -GNinja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM base AS dev
WORKDIR /src
CMD ["bash"]
```

配套 `docker-compose.yml`：

```yaml
services:
  dev:
    build:
      context: .
      target: dev
    volumes:
      - .:/src
    stdin_open: true
    tty: true

  build:
    build:
      context: .
      target: builder
```

**验收标准**: `docker compose run dev` 可进入完整开发环境，`docker compose build` 可构建项目。

---

### 3.2 QFluentKit 依赖解耦评估

**背景**: 项目从 QFluentKit 仅使用了 SVG 图标资源（32 个文件），未使用其实际 Widget 组件。当前 QFluentKit 以 SHARED 库方式构建，POST_BUILD 复制 DLL。

**评估任务**:

1. **确认依赖范围**:
   - 搜索所有源码中对 `QFluent` 命名空间或头文件的引用
   - 如果仅使用了图标资源，则可以完全解耦

2. **方案 A — 内嵌图标**（推荐，如果确实只用图标）:
   ```bash
   # 将 32 个 SVG 复制到 app/res/icons/fluent/ 下
   cp third_party/QFluentKit/QFluent/res/images/icons/*.svg app/res/icons/fluent/
   ```
   - 更新 `ide.qrc` 中的路径
   - 从 CMakeLists.txt 移除 `add_subdirectory(third_party/QFluentKit)`
   - 移除 POST_BUILD 复制命令
   - 移除 `target_link_libraries(... QFluent)`

3. **方案 B — Fork 并 Pin**（如果未来可能使用 QFluent Widget）:
   - Fork QFluentKit 到项目的 `third_party/QFluentKit`
   - Pin 到固定 commit
   - 在 `CMakeLists.txt` 中记录版本号

**验收标准**: 要么移除 QFluentKit 运行时依赖，要么明确记录保留理由。

---

### 3.3 scripts/ 去硬编码路径

**文件**: `scripts/_common.bat`

**现状**: 硬编码了 `D:\qt` 和 `C:\qt` 路径。

**任务**:

```bat
@echo off
REM --- 检测 Qt6 路径 ---
if not defined QTDIR (
    REM 方法1: 环境变量（最优先）
    if defined QTDIR goto :qt_found
    
    REM 方法2: qmake 在 PATH 中
    for /f "delims=" %%v in ('where qmake 2^>nul') do (
        for /f "delims=" %%q in ('%%v -query QT_INSTALL_PREFIX') do set "QTDIR=%%q"
    )
    
    REM 方法3: 常见安装路径扫描
    if not defined QTDIR (
        for /d %%p in ("C:\Qt\6.*\msvc2022_64" "D:\Qt\6.*\msvc2022_64") do (
            if exist "%%p\lib\cmake\Qt6" set "QTDIR=%%p"
        )
    )
)
:qt_found
if not defined QTDIR (
    echo [ERROR] Qt6 not found. Set QTDIR or add qmake to PATH.
    exit /b 1
)
echo [INFO] Qt6: %QTDIR%
```

**验收标准**: 在不同 Qt 安装路径的开发者机器上均可正常工作。

---

### 3.4 Logo SVG 元数据补充

**文件**: `app/res/icons/minilang_logo.svg` 和 `minilang_logo_dark.svg`

**任务**: 在两个文件的 `<svg>` 标签后添加：

```svg
<title>MiniLang Logo</title>
<desc>ML monogram in gradient blue, representing the MiniLang programming language IDE</desc>
```

**验收标准**: SVG 预览工具可显示标题和描述。

---

### 3.5 CI 性能基准回归检测

**目标**: 防止代码变更导致执行效率退化。

**任务**:

1. 创建 `tests/TestPerformanceRegression.cpp`：
   - 定义一组固定的 MiniLang 源码（fib(30)、大循环、字符串拼接等）
   - 使用 `measureInterpreterOnce` / `measureStackVMOnce` / `measureRegisterVMOnce` 测量执行时间
   - 将结果写入 JSON 文件

2. 在 CI 中添加 benchmark job：
   - 运行性能基准测试
   - 与上次 main 分支的结果对比
   - 如果退化超过 20% 则报警（不阻塞 CI，仅 Comment 到 PR）

3. （可选）使用 GitHub Actions Cache 存储历史基准数据

**验收标准**: PR 时可看到性能对比评论。

---

## 四、遗留 P1 问题修复（上一轮审计发现）

### 4.1 Sanitizer 跨平台补全

**文件**: `CMakeLists.txt` 第 308–312 行

**现状**: 仅处理 MSVC + address，GCC/Clang 和 undefined sanitizer 完全缺失。

**修复**:

```cmake
# ============================================================
# Sanitizer 支持（跨平台）
# 用法: cmake -DMINILANG_SANITIZE=address ...
# ============================================================
set(MINILANG_SANITIZE "" CACHE STRING "Enable sanitizer: address, undefined")
if(MINILANG_SANITIZE)
    if(MINILANG_SANITIZE STREQUAL "address")
        if(MSVC)
            target_compile_options(minilang_compile_options INTERFACE /fsanitize=address)
            target_link_options(minilang_compile_options INTERFACE /fsanitize=address)
        else()
            # GCC / Clang
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=address)
        endif()
    elseif(MINILANG_SANITIZE STREQUAL "undefined")
        if(NOT MSVC)  # UBSan 在 MSVC 上支持有限
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=undefined -fno-omit-frame-pointer)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=undefined)
        else()
            message(WARNING "MINILANG_SANITIZE=undefined has limited support on MSVC")
        endif()
    elseif(MINILANG_SANITIZE STREQUAL "both")
        if(MSVC)
            target_compile_options(minilang_compile_options INTERFACE /fsanitize=address)
            target_link_options(minilang_compile_options INTERFACE /fsanitize=address)
        else()
            target_compile_options(minilang_compile_options INTERFACE
                -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(minilang_compile_options INTERFACE -fsanitize=address,undefined)
        endif()
    else()
        message(FATAL_ERROR "Unknown MINILANG_SANITIZE value: ${MINILANG_SANITIZE}"
                " Supported: address, undefined, both")
    endif()
endif()
```

**验收标准**: `cmake -DMINILANG_SANITIZE=address` 在 Linux/macOS 上正常工作。

---

### 4.2 根目录 .obj 文件清理

**位置**: 项目根目录

**现状**: 存在 5 个 `.obj` 文件（旧 `.vcxproj` 构建遗留）：
- `VMContainers.obj`, `VMCalls.obj`, `VM.obj`, `Value.obj`, `TypeChecker.obj`

**任务**:
1. 确认这些文件不在任何构建配置中被引用
2. 删除它们
3. 确认 `.gitignore` 已包含 `*.obj` 规则（已有 ✅）

```bash
cd /mnt/local/ide
rm -f VMContainers.obj VMCalls.obj VM.obj Value.obj TypeChecker.obj
```

**验收标准**: 根目录不再有 `.obj` 文件。

---

### 4.3 CMakeLists.txt CPack 语法错误修复

**文件**: `CMakeLists.txt` 第 330 行附近

**现状**: `endif()` 后面多了一对空括号 `()`，且非 Windows 平台缺少 CPACK_GENERATOR 设置。

**修复**:

```cmake
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
endif()
# 删除这行后面的 ()
```

**验收标准**: `cpack -G` 在所有平台上都有合理的默认值。

---

## 五、QSS 双主题对称性修复清单

**文件**: `styles.qss` vs `styles_dark.qss`

以下属性在两套样式表中不对称，需逐一修复：

### 5.1 `QToolBar::separator` height 不一致

| | Light (`styles.qss`) | Dark (`styles_dark.qss`) |
|---|---|---|
| 行号 | L289-L293 | L282-L286 |
| width | `width: 1px` | `width: 1px` ✅ |
| **height** | **`height: 1px`** | **❌ 缺失** |
| margin | `margin: 8px 4px` | `margin: 6px 4px` |

**修复**: 在 `styles_dark.qss` 的 `QToolBar::separator` 中添加 `height: 1px;`

### 5.2 `QToolBar` fallback padding 不一致

| | Light | Dark |
|---|---|---|
| 行号 | L218-L225 | L220-L227 |
| padding | `padding: 4px 4px` | `padding: 6px 8px` |

**建议**: 统一为 `padding: 4px 8px`（暗色的水平值更合理，亮色的垂直值更紧凑）

### 5.3 `QToolBar QToolButton` padding/border 不一致

| 属性 | Light (L104-113) | Dark (L228-235) |
|---|---|---|
| padding | `padding: 0` | `padding: 6px 12px` |
| border | 无 | `border: 1px solid transparent` |
| hover color | 未设置 | `color: #ffffff` |

**建议**: 统一为 `padding: 0; border: none;`（保持紧凑），hover 颜色两套都显式声明。

### 5.4 `QTextEdit#outputEdit` 缺少 border-top（暗色）

| | Light (L638-646) | Dark (L617-624) |
|---|---|---|
| border-top | **`border-top: 1px solid #e8e8e8;`** | **❌ 缺失** |

**修复**: 在 `styles_dark.qss` 的 `QTextEdit#outputEdit` 中添加：
```css
border-top: 1px solid #2d2d30;
```

### 5.5 `QListWidget#errorList` 缺少 border-top（暗色）

| | Light (L649-658) | Dark (L627-635) |
|---|---|---|
| border-top | **`border-top: 1px solid #e8e8e8;`** | **❌ 缺失** |

**修复**: 同上，添加 `border-top: 1px solid #2d2d30;`

### 5.6 `QPushButton#welcomePrimaryBtn` 缺少 icon-size（暗色）

| | Light (L380-389) | Dark (L371-380) |
|---|---|---|
| icon-size | **`icon-size: 16px;`** | **❌ 缺失** |

**注意**: `icon-size` 不是标准 Qt SS 属性。正确的做法是：
- 两套 QSS 都**移除** `icon-size` 属性
- 在 C++ 代码中使用 `button->setIconSize(QSize(16, 16))` 设置

### 5.7 非标准属性清理

**位置**: 两套 QSS 多处

| 属性 | 位置 | 处理 |
|---|---|---|
| `icon-size: 16px` | L388, L408 (light) | 移除，改用 C++ `setIconSize()` |
| `qproperty-drawBase: 0` | L514 (light) | 确认 Qt 版本 ≥5.0，否则移除 |

---

## 附录：执行顺序建议

如果由 AI Agent 一次性执行，建议按以下顺序操作（考虑依赖关系）：

```
Phase 1 — 快速修复（无依赖，预计 30 分钟）
├── 4.2 清理根目录 .obj 文件
├── 4.3 修复 CPack 语法错误
├── 3.4 补充 Logo SVG 元数据
└── 5.x QSS 对称性修复（7 个小项）

Phase 2 — 构建系统改进（预计 1 小时）
├── 4.1 Sanitizer 跨平台补全
├── 1.2 CMakePresets 补充
├── 3.3 scripts 去硬编码
└── 3.2 QFluentKit 解耦评估

Phase 3 — CI/CD 改进（预计 1 小时）
├── 1.1 CI 跨平台支持
├── 2.3 覆盖率集成
└── 3.5 性能基准检测

Phase 4 — 内容创建（预计 2-3 小时）
├── 1.3 翻译文件填充
├── 1.4 README 截图
├── 2.1 示例程序扩充
├── 2.4 CONTRIBUTING.md
└── 2.5 文档拆分重构

Phase 5 — 清理收尾（预计 30 分钟）
├── 2.2 测试命名统一
├── 3.1 Docker 环境
└── 最终验证：全量测试通过 + CI 绿色
```

---

> 文档版本: v1.0 | 生成日期: 2026-07-05 | 基于 AGENTS.md 审计协议 + 非源码文件审计 + 全面工程化审查
