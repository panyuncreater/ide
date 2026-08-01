# MiniLang 静态分析接入（阶段六 · `testing/static-analysis/`）

本目录是自动化 bug 挖掘流水线的**静态分析**环节，聚焦「找 bug」的检查组，
**只报告不修改**——修复由维护者决定。全部为新增工具/配置代码，**不改动**编译器实现，
也**不改动根目录 `.clang-tidy`**（那是 CI 信息性检查、且带 `WarningsAsErrors` 会阻塞主构建）。

| 文件 | 作用 |
|------|------|
| `.clang-tidy` | 静态分析专用配置：聚焦 `bugprone-*` / `clang-analyzer-*` / `cppcoreguidelines-*`，无 `WarningsAsErrors`。 |
| `sa_common.py` | 公共库：诊断解析 / 去重 / 严重度分级 / 一句话解释 / Markdown 报告。 |
| `run_clang_tidy.py` | clang-tidy 全量分析驱动（净化 MSVC 编译库 → 并行分析 → Top 20 报告）。 |
| `run_clazy.py` | clazy（Qt 专用）分析驱动；未安装时输出如实前置条件报告。 |
| `reports/` | 生成的报告：`clang-tidy-report.md` / `clazy-report.md`（+ `*-findings.jsonl` 原始数据）。 |

前置：已用 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 配置出 `compile_commands.json`
（`build_debug/` 已具备）。Python 3.8+，无第三方依赖。

---

## 1. clang-tidy

### 1.1 安装

推荐用 pip 的 clang-tidy wheel（自包含、免管理员、跨平台，内置 LLVM 官方二进制）：

```bash
py -3 -m pip install clang-tidy      # Windows（py 启动器指向带 pip 的 Python）
python3 -m pip install clang-tidy    # Linux/macOS
```

也可用系统包管理器（`apt install clang-tidy` / `brew install llvm`）或 LLVM 官方安装包。
驱动按 `MINI_CLANG_TIDY 环境变量 > PATH > 常见目录` 顺序定位，可用 `--clang-tidy` 显式指定。

### 1.2 运行（Windows / MSVC）

> **必须在 VS DevShell 中运行**：clang-tidy 以 cl 驱动模式解析 MSVC/SDK 系统头，
> 依赖 `INCLUDE` 环境变量。否则大量 TU 会因缺系统头而解析失败。

```powershell
# 1) 初始化 MSVC 环境（单条命令，避免跨会话丢失）
Import-Module "<VS>\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "<VS>" -Arch amd64 -SkipAutomaticLocation
# 2) 全量分析（默认 build_debug，jobs=CPU 核数）
python testing/static-analysis/run_clang_tidy.py
# 局部/冒烟：
python testing/static-analysis/run_clang_tidy.py --file-filter compiler/ --timeout 120
```

### 1.3 MSVC 编译库净化（为何需要）

`compile_commands.json` 里是 `cl.exe` 命令，含 clang 前端无法消化的 MSVC 专有开关：
`/Yu`（用 PCH）、`/Fp<pch>`（PCH 文件，clang 读不了 MSVC PCH 格式）、`/Zf`、`/WX` 等。
`run_clang_tidy.py` 会**净化**这些开关写入临时编译库（`.cache-clang-tidy/`），
但**保留 `/FI cmake_pch.hxx`**（强制包含），使项目头文件仍能被 clang 正常解析。

### 1.4 配置聚焦（`.clang-tidy`）

启用三组，并关闭少数本代码库中高噪声/低信号项（NaN-boxing 位运算、定长数组下标、
`operator[]` 未检查访问、X-Macro 等，均属刻意设计）。其余 `cppcoreguidelines-*`
（`init-variables` / `pro-type-*` / `special-member-functions` / `owning-memory` / `slicing`…）
全部保留，因其常指向真实生命周期/初始化问题。纯报告用途，不设 `WarningsAsErrors`。

---

## 2. clazy（Qt 专用检查）

clazy 是构建在 clang 之上的 **Qt 语义检查器**（如 QString 冗余分配、range-for 隐式
detach、`connect` 误用、容器反模式等），`clazy-standalone` 消费 `compile_commands.json`，
用法与 clang-tidy 一致。

### 2.1 安装

clazy 需匹配的 Clang，**推荐 Linux/WSL**（Windows/MSVC 无成熟发行版，需自行构建）：

| 平台 | 方式 |
|------|------|
| Ubuntu/Debian | `sudo apt-get install -y clazy`（提供 `clazy-standalone`） |
| Fedora | `sudo dnf install clazy` |
| macOS | `brew install clazy` |
| 源码 | 见 https://github.com/KDE/clazy （需对应版本的 LLVM/Clang 开发包） |
| Windows | 官方无预编译包；建议在 WSL 中安装并对 Linux 侧构建目录运行 |

驱动按 `MINI_CLAZY > PATH > 常见目录` 定位，可用 `--clazy` 指定 `clazy-standalone`。

### 2.2 `CLAZY_CHECKS` 推荐级别

clazy 检查按等级递进，误报随等级升高而增多：

| 等级 | 含义 | 建议 |
|------|------|------|
| `level0` | 最保守，几乎无误报（明确 bug / 崩溃类） | 最低门槛 |
| **`level1`** | **官方推荐**：level0 + 常见性能/正确性反模式，误报可控 | **CI / 常规审计默认（本驱动默认）** |
| `level2` | 更激进，更多可疑模式，误报增多 | 深度专项审计 |
| `level3` | 实验/极噪声 | 仅偶尔定向排查 |

也可显式列检查：`CLAZY_CHECKS="level1,no-qstring-allocations,detaching-member"`。

### 2.3 运行

```bash
# Linux/WSL（clazy 已装 + clang 生成的 compile_commands.json）
CLAZY_CHECKS=level1 python3 testing/static-analysis/run_clazy.py --build-dir <linux-build>
# 或指定等级 / 可执行
python3 testing/static-analysis/run_clazy.py --checks level2 --clazy $(which clazy-standalone)
```

若本机未安装 clazy，`run_clazy.py` 会在 `reports/clazy-report.md` 输出一份**如实的
前置条件报告**（不伪造 findings），并给出上述安装与运行命令。

---

## 3. 报告解读

两份报告（`reports/clang-tidy-report.md` / `reports/clazy-report.md`）结构一致：

- **元信息**：工具/版本/配置/范围/TU 数（成功/超时/解析失败）/耗时。
- **分级统计**：按严重度 high / medium / low 汇总（分级规则见 `sa_common.severity_of`）。
- **Top 20 问题清单**：`文件:行号` + 检查项 + 一句话解释；按严重度→命中数排序，
  同一检查项最多列若干条（多样化，避免单一高频检查霸榜）。
- **按检查项汇总 / 命中最多文件**：便于定位面。

严重度分级为**辅助排序启发式**（clang-analyzer 核心/C++/内存组、use-after-move、
dangling、未初始化等归 high），不代表最终定性；**是否修复由维护者判断**。

> 本环节遵循流水线总约束：只报告、不改编译器实现，不改根 `.clang-tidy`。
