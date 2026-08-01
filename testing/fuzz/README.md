# 阶段四：前端鲁棒性 Fuzzing（`testing/fuzz/`）

本目录是自动化 bug 挖掘流水线的**阶段四**：用 **libFuzzer**（Clang 自带）对
MiniLang 的**词法 + 语法分析器**做覆盖率引导的模糊测试。

被测不变量（front-end robustness invariant）：

> **任意字节流 → 完整前端（`Lexer::scan` → `Parser::parse`，含 parse 期宏展开）
> → 不得崩溃**（无段错误 / abort / UB / 未处理 C++ 异常 / 死循环）。

全部为**新增测试/工具代码**，不修改编译器任何实现逻辑（仅编译既有源文件）。

---

## 0. 关键结论：前端可直接链接，无需重构

排查 fuzz target 与前端的链接可行性后确认：**前端已具备可直接链接的库接口，无需任何重构。**

- 公共 API：`Lexer::scan(const std::string&) → std::vector<Token>`、
  `Parser::parse(const std::vector<Token>&) → std::unique_ptr<Block>`（均为 public）。
- **最小链接闭包 = 6 个既有编译器源文件**（全部原样编译，不改动）：

  | 源文件 | 作用 |
  |--------|------|
  | `lexer/Lexer.cpp`         | 词法分析 |
  | `parser/Parser.cpp`       | 语法分析（含 parse 期宏展开调用） |
  | `ast/ASTNode.cpp`         | AST 节点 vtable + `getValue()/nodeName()` |
  | `ast/MacroExpander.cpp`   | 宏展开 |
  | `interpreter/Value.cpp`   | `Value::toStringImpl/equalsImpl`（被 `nodeName()` 触及） |
  | `interpreter/GcManager.cpp` | `RefCounted::~RefCounted`（Value 堆对象析构链） |

- **无 Qt / 无 windows.h / 无 asmjit 依赖**：parse-only 闭包不引用任何 Qt 符号，
  可脱离 `minilang_core`（及其 `Qt6::Core` 依赖）独立编译链接。
- 该闭包的**链接完备性已由 MSVC 构建 `fuzz_parser_replay` target 证实**（零未解析符号）。

> 因此原任务「若前端无独立库接口需列最小重构清单」这一分支**不触发**——重构清单为空。

---

## 1. 组成

| 文件 | 作用 |
|------|------|
| `../fuzz_parser.cpp` | **双模式 fuzz target**（位于 `testing/`，见任务指定路径）。libFuzzer 模式提供 `LLVMFuzzerTestOneInput`；定义 `MINILANG_FUZZER_STANDALONE` 后提供 `main()` 做回放回归。 |
| `build_fuzz.sh`   | Clang + `-fsanitize=fuzzer,address,undefined` 编译上述 6 个源文件 + target，产出可执行 `fuzz_parser`。 |
| `run_fuzz.sh`     | 运行 fuzzer（**默认 5 分钟**）；结束后 crash 用例**自动归档**到 `crashes/archive_<时间戳>/`。 |
| `collect_corpus.sh` | 收集仓库 `samples/` 下全部 `*.mini` 示例源程序为初始种子语料。 |
| `fuzz_parser.dict` | libFuzzer 词典：MiniLang 关键字/运算符/插值边界，加速构造深入 Parser 的输入。 |
| `corpus/`         | **初始种子语料**（已提交，15 个示例源程序，由 `collect_corpus.sh` 生成）。 |
| `corpus_work/`    | 运行期语料（由 `run_fuzz.sh` 从 `corpus/` 播种并增长，已 gitignore）。 |
| `crashes/`        | crash/leak/timeout 用例归档目录（`archive_*` 已 gitignore，保留 `.gitkeep`）。 |

---

## 2. 环境依赖

- **Clang**（自带 libFuzzer）。目标环境为 **WSL/Linux/macOS** 或支持
  `-fsanitize=fuzzer` 的 clang-cl。
  - Ubuntu/WSL：`sudo apt-get install clang`
  - 验证：`clang++ --version`
- **MSVC 无 libFuzzer**：Windows 上请用 §5 的 CTest standalone 回放守护该 target。

---

## 3. 快速上手（Clang 环境）

```bash
cd testing/fuzz

# 一步到位：构建 + 收集种子 + 跑 5 分钟 + 自动归档 crash
./run_fuzz.sh

# 只跑 60 秒
./run_fuzz.sh -t 60

# 强制重新构建后再跑
./run_fuzz.sh --rebuild

# -- 之后的参数原样透传给 libFuzzer（如并行 4 job）
./run_fuzz.sh -- -jobs=4 -workers=4
```

单独构建 / 收集：

```bash
./build_fuzz.sh                       # 仅构建 fuzz_parser
CXX=/usr/bin/clang++-18 ./build_fuzz.sh   # 指定编译器
SANITIZERS=fuzzer,address ./build_fuzz.sh # 自定义 sanitizer 组合
./collect_corpus.sh                   # 仅（重新）收集种子到 corpus/
```

底层编译命令（`build_fuzz.sh` 等价形式，供参考）：

```bash
clang++ -std=c++20 -O1 -g \
    -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined \
    -fsanitize-address-use-after-scope \
    -fno-sanitize-recover=all \
    -I<repo> -I<repo>/common -include <repo>/common/pch.h \
    <repo>/lexer/Lexer.cpp <repo>/parser/Parser.cpp \
    <repo>/ast/ASTNode.cpp <repo>/ast/MacroExpander.cpp \
    <repo>/interpreter/Value.cpp <repo>/interpreter/GcManager.cpp \
    <repo>/testing/fuzz_parser.cpp \
    -o fuzz_parser
```

> `-I<repo>/common` 不可省略：`lexer/Lexer.h` 与 `parser/Parser.h` 直接
> `#include "Diagnostic.h"`（位于 `common/`）。`-include common/pch.h` 镜像主构建
> 的 PCH 标准库可见性。

---

## 4. Crash 分类与复现

`run_fuzz.sh` 结束后：

- 若发现 crash/leak/timeout/oom，libFuzzer 将用例写入 `crashes/`，脚本随即把它们
  连同 `fuzz.log` 归档到 `crashes/archive_<时间戳>/`，并打印复现命令。
- 复现单个用例（直接把用例喂给 target）：

  ```bash
  ./fuzz_parser crashes/archive_<时间戳>/crash-<sha1>
  ```

  ASan/UBSan 栈回溯会直接指向前端崩溃点（Lexer/Parser/AST/Value）。
- 每个归档用例即一个**最小可复现字节流**，可交给阶段五（用例缩减）进一步缩小。

---

## 5. 无 Clang 平台：MSVC standalone 回放（CTest 守护）

为了在 **Windows/MSVC**（无 libFuzzer）上持续守护该 fuzz target 不腐化，同一份
`fuzz_parser.cpp` 在定义 `MINILANG_FUZZER_STANDALONE` 后编译为 `fuzz_parser_replay`：
`main()` 逐个回放 `corpus/` 种子 + `samples/mini/` 示例 + 一组内置边界字节样例，
**进程存活到底即通过**（任一输入使前端崩溃 → 进程崩溃 → CTest 判失败）。

该回归 target 直接编译**与 `build_fuzz.sh` 完全一致的 6 源文件闭包**，因此本构建同时
验证该闭包在 MSVC 下**链接完备、无 Qt 依赖**。

```powershell
# 已启用 MINILANG_BUILD_TESTING_PIPELINE=ON 的构建树中
cmake --build build_debug --target fuzz_parser_replay
ctest --test-dir build_debug -R "pipeline.fuzz.replay" --output-on-failure

# 手动回放任意目录/文件
build_debug\testing\fuzz_parser_replay.exe testing\fuzz\corpus samples\mini
```

---

## 6. 种子语料

`corpus/` 由 `collect_corpus.sh` 从 `<repo>/samples/` 收集全部 `*.mini`（15 个示例源
程序），文件名按相对路径扁平化（`/` → `__`）避免同名冲突。新增示例后重跑
`collect_corpus.sh` 即可刷新。libFuzzer 会在 `corpus_work/` 上做语料最小化与增长，
`corpus/` 始终保持为纯净种子。

---

## 7. 本仓库环境下的验证状态

| 验证项 | 结果 |
|--------|------|
| `fuzz_parser.cpp` 在 MSVC `/W4 /WX` 下编译 | ✅ 通过 |
| 6 源文件闭包在 MSVC 下链接（无未解析符号、无 Qt） | ✅ 通过 |
| 回放 `corpus/` + `samples/mini/` + 内置边界样例（45 次前端执行） | ✅ 全部存活，无崩溃 |
| CTest `pipeline.fuzz.replay` | ✅ Passed |
| 三个 `.sh` 脚本 `bash -n` 语法检查 | ✅ 通过 |
| 实际 `-fsanitize=fuzzer` 构建 + 活体 fuzzing | ⏳ 需 Clang 环境（本机 Windows/WSL 均未装 Clang），按 §3 在 WSL/Linux 执行 |
