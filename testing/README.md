# MiniLang 自动化 Bug 挖掘流水线（`testing/`）

本目录是**与主工程解耦**的自动化编译器 bug 挖掘流水线，全部为**新增测试/工具代码**，
不修改编译器任何实现逻辑。默认不参与主工程/CI 构建，需显式启用。

> 当前进度：**阶段一至阶段六全部落地**——阶段一（测试基建）、阶段二（生成式差分
> 核心）、阶段三（Sanitizer 矩阵）、阶段四（前端 Fuzzing）、阶段五（用例自动缩减）、
> 阶段六（静态分析接入）。CI 夜间部署见 §14。

---

## 0. 核心事实（务必先读）

MiniLang 与经典「编译到可执行文件」的编译器**不同**：

- **没有 `run`/`eval` 子命令，也不产出独立 a.out**。源码是**进程内**执行的。
- 有 **4 条执行后端**（本流水线细分为 **5 种执行配置**）：

  | 配置名 | 含义 |
  |--------|------|
  | `interp`     | 树遍历解释器 |
  | `stackvm`    | 栈式字节码 VM（非 IR 路径） |
  | `stackvm-ir` | 栈式字节码 VM（IR 路径） |
  | `regvm`      | 寄存器式 VM（IR 路径） |
  | `jit`        | x86-64 JIT（不支持场景**优雅降级**，不计入差分投票） |

- **源语言是自研 MiniLang（非 C 子集）**，GCC/Clang 无法作参考编译器。
  因此差分 oracle = **多后端交叉差分**（项目核心不变量：各后端语义严格一致）。

---

## 1. 组成

| 文件 | 作用 |
|------|------|
| `runner/mini_diff_runner.cpp` | C++ 执行内核：单文件×单后端进程内执行，输出结构化 JSON，含编译/运行双阶段超时看门狗。链接 `minilang_core`。 |
| `driver.py` | Python 驱动：调用 runner，双重超时，产出 `{backend, compile_ok, run_ok, exit_code, status, stdout_hash, stderr, ...}`；并提供 `diff` 多后端一致性检查。可被后续阶段作为库导入。 |
| `generator.py` | **（阶段二）** 确定性 MiniLang 随机程序生成器（Csmith 思想）：无运行时错误、末尾输出校验和、同种子同程序、特性开关。 |
| `diff_test.py` | **（阶段二）** 四后端差分投票核心：单文件 / 批量 `--n`、分类统计、命中落盘、按签名去重、Markdown 报告。 |
| `reduce.py` | **（阶段五）** C-Reduce 式最小化：趣味性判定驱动，行/块删除+表达式简化+重命名迭代缩减；含 `verify` 子命令供 CTest 回归。 |
| `regression/` | **（阶段五）** 缩减后最小复现用例 + `manifest.json`（按 `open/fixed` 管理，驱动 `pipeline.regression.*` CTest）。 |
| `static-analysis/` | **（阶段六）** clang-tidy / clazy 接入：专用 `.clang-tidy`、驱动脚本、报告（见该目录 `README.md`）。 |
| `CMakeLists.txt` | 构建 `mini_diff_runner` + 注册 `pipeline.diff.*`（含生成式 `batch_smoke`）与 `pipeline.regression.*` CTest。 |

---

## 2. 环境依赖

- 已能构建 MiniLang 主工程（MSVC 19.51+ / GCC 13+ / Clang 16+，Qt 6.x，CMake ≥ 3.25，Ninja）。
- Python 3.8+（无第三方依赖）。

---

## 3. 构建

流水线默认关闭，需显式开启 `-DMINILANG_BUILD_TESTING_PIPELINE=ON`。

### Windows（复用现有 `build_debug` 构建树，MSVC）

```powershell
# 在已初始化的 VS DevShell 中（见根目录 minilang-build Skill）
cmake -S . -B build_debug -DMINILANG_BUILD_TESTING_PIPELINE=ON
cmake --build build_debug --target mini_diff_runner
```

### 使用 Preset（推荐）

```bash
cmake --preset windows-msvc-debug -DMINILANG_BUILD_TESTING_PIPELINE=ON
cmake --build out/build/debug --target mini_diff_runner
```

构建产物：`<build>/testing/mini_diff_runner(.exe)`。

---

## 4. 用法

### 4.1 单后端执行（`run`）

```bash
python testing/driver.py run --file samples/mini/01-basics/hello.mini --backend interp --pretty
```

输出（示例，节选）：

```json
{
  "backend": "interp",
  "status": "ok",
  "compile_ok": true,
  "run_ok": true,
  "degraded": false,
  "exit_code": 0,
  "output_len": 210,
  "output_hash": "…",
  "diff_hash": "…",
  "output": "=== 基础输出 ===\nHello, MiniLang!\n…",
  "error": ""
}
```

`status` 取值：`ok` / `parse_error` / `compile_error` / `runtime_error` /
`jit_unsupported`（优雅降级）/ `jit_runtime_error` / `crash` / `hang` / `io_error`。

退出码：`ok` 或 `jit_unsupported` → 0；其余 → 1。`--assert-status <s>` 可用于回归断言。

### 4.2 多后端一致性检查（`diff`）

```bash
python testing/driver.py diff --file samples/mini/06-object-oriented/oop_shapes.mini --quiet
```

- 对全部 5 种配置执行，比较「规范输出串」（print 输出 + 统一错误标记）。
- `jit` 因不支持而 `degraded` 时**自动排除**出投票集合（不误报为分歧）。
- 分类 `category`：`agree` / `disagreement`（含少数派 `suspects`）/ `crash` / `hang` /
  `degraded_only` / `all_error`。
- `--require-agreement`：仅 `agree`/`degraded_only` 判为通过（退出 0），供 CTest 使用。

### 4.3 自动定位

`driver.py` 自动探测 runner 与 Qt 运行期 DLL：

- runner：`--runner` > 环境变量 `MINI_DIFF_RUNNER` > 常见构建目录（`build_debug/`、
  `out/build/*/`…）下的 `testing/mini_diff_runner`。
- Qt DLL：`--qt-bin` > `MINI_QT_BIN` > 从构建树 `CMakeCache.txt` 的 `CMAKE_PREFIX_PATH` 推导。

若报「找不到 Qt6Cored.dll」，显式指定 `--qt-bin <Qt>/msvc2022_64/bin` 即可。

---

## 5. CTest 回归

启用流水线后，`samples/mini` 下可独立运行的示例被注册为差分自洽回归用例（前缀 `pipeline.diff.*`）：

```bash
ctest --test-dir build_debug -L pipeline --output-on-failure
# 或仅跑差分用例
ctest --test-dir build_debug -R "pipeline.diff" --output-on-failure
```

> 注：`08-modules-and-backends/` 依赖真实文件模块解析（runner 注入空模块 loader），暂不纳入自动用例。

---

## 6. 超时与 HANG 归类

- runner 内部看门狗对**编译**与**运行**两阶段分别计时（默认各 10s），超时打印
  `status=hang, phase=compile|run` 并硬退出（退出码 124）。
- `driver.py` 另设子进程整体超时（compile+run+5s）作为兜底。
- 二者任一触发即归类为 **HANG 类 bug**。

---

## 7. 产物目录（后续阶段）

- `testing/artifacts/`：差分命中案例（种子 + 命令行 + 原始输出）— **阶段二**引入。
- `testing/regression/`：缩减后的最小复现用例 + `manifest.json` — **阶段五**已落地。
- `testing/static-analysis/reports/`：clang-tidy / clazy 报告 — **阶段六**已落地。
- `testing/fuzz/`：libFuzzer 语料与 crash — **阶段四**引入。

---

## 8. 已知限制（阶段一）

- 阶段一的 `diff` 仅做 plumbing 级自洽检查；**完整的批量投票、命中落盘、Markdown 报告**在阶段二 `diff_test.py` 实现。
- `jit` 目前对多数复杂语法优雅降级，差分覆盖以 `interp/stackvm/stackvm-ir/regvm` 四配置为主。
- Sanitizer（ASan/UBSan）、libFuzzer、clang-tidy/clazy 依赖 Clang 工具链，目标环境为 **WSL/Linux**（见后续阶段）。

---

## 9. 阶段一验证中发现的真实跨后端差异（已确认，非误报）

仅搭建基建、跑通 5 个独立样例，即命中 **两类真实的 strict 后端语义不一致**（`interp` 与
编译后端分歧）。已用最小用例确认，**未修改任何编译器代码**（按约束仅报告）。对应
CTest 用例以 `WILL_FAIL` 标记（`known_divergence` 标签），修复后会自动转正。

### 发现 1 — 循环/块作用域内命名函数无法在编译后端解析（P1 语义不一致）

最小复现（`testing/artifacts/_repro_closure.mini`）：

```
var fns = [];
for (var i = 0; i < 3; i = i + 1) {
    fun getter() { return i; }
    fns.push(getter);
}
print(fns[0]());
```

| 后端 | 结果 |
|------|------|
| `interp` | 输出 `3` ✅ |
| `stackvm` / `stackvm-ir` / `regvm` / `jit` | 运行时错误 `未定义的变量: getter` ❌ |

树遍历解释器把「块内命名函数声明」当作块作用域变量绑定；四个编译后端在编译该
构造时未登记该名字，运行到 `fns.push(getter)` 时报未定义变量。四后端独立复现同一
错误、仅 interp 不同，指向编译前端/各后端对「块内 `fun` 声明」的处理缺失。

### 发现 2 — 内建方法在各 VM 后端覆盖不一致（P2 一致性）

样例 `samples/mini/05-data-structures/data_structures.mini` 上：

| 后端 | 结果 |
|------|------|
| `interp` | 完整输出（1628 字节）✅ |
| `stackvm` / `stackvm-ir` | 运行时错误：缺 `remove` |
| `regvm` | 运行时错误：缺 `contains`（比 stackvm 更早失败） |
| `jit` | 优雅降级：缺 `array.len` |

不同后端缺失不同的容器内建方法，导致同一程序在各后端行为分歧。

> 这两项属项目 `AGENTS.md` 已列的高频 bug 模式（三后端语义不一致 / 闭包生命周期）。
> 系统化的批量投票与命中落盘见下阶段二；用例缩减将在**阶段五**展开。

---

## 10. 阶段二：差分测试核心（生成器 + 四后端投票 + 报告）

### 10.1 随机程序生成器 `generator.py`

严格模仿 Csmith：只生成**语义确定、无运行时错误**的合法 MiniLang 程序，末尾 `print(cs)`
输出全局校验和作为差分依据。**同种子必生成逐字节相同的程序**。

```bash
python testing/generator.py --seed 42                 # 打印到 stdout
python testing/generator.py --seed 42 --out prog.mini # 写入文件
python testing/generator.py --seed 42 --statements 40 --max-depth 4 --no-arrays
```

特性开关：`--statements`、`--globals`、`--max-depth`、`--max-loop-iters`、
`--no-loops`、`--no-functions`、`--no-arrays`、`--no-dicts`。

**无运行时错误不变量**（保证可复现且不被已知 bug 淹没）：所有整型值恒被规范到
`[0, MOD)`；除法/取模除数恒为正小常量（不除零）；乘法仅「变量×小常量」（不溢出提升
为 float）；`and/or` 仅作用于布尔比较；仅顶层声明 `fun`（规避发现 1）；只用内建
`len()/sum()` 而非 `.remove()/.contains()`（规避发现 2）；**字典**（dict）值恒为
规范化 int、键为确定字符串集合，索引/values 索引仅用存活键（生成器跟踪 set/remove
后的键集合），get 恒带默认值，has/contains 存在性由生成器确定——且 remove 仅生成在
顶层直接语句（无条件执行恰一次）、新键仅生成在必然执行上下文（顶层/循环体），
保证运行时键集合与生成期静态集合一致，绝不触发键缺失/空 values 索引。

### 10.2 差分测试 `diff_test.py`

```bash
# 单文件（已有 .mini）
python testing/diff_test.py --file samples/mini/06-object-oriented/oop_shapes.mini

# 批量：连续 1000 个种子，遇错继续，末尾输出分类统计 + 报告
python testing/diff_test.py --n 1000 --seed 1

# 自定义生成剖面 + 产物写到指定目录
python testing/diff_test.py --n 200 --seed 1 --statements 20 --out-dir /tmp/mlfuzz
```

退出码：无命中→ 0；有命中→ 1（`--no-fail` 强制 0）。

### 10.3 分类与报告解读

投票模型：`interp/stackvm/stackvm-ir/regvm` 为 **strict**（必须一致），JIT 为可选加分项。

| 内部类别 | 含义 |
|----------|------|
| `agree` | 四 strict 后端逐字节一致（通过） |
| `disagreement` | strict 后端之间分歧（真实 bug；含少数派 `suspects`、`compile`/`runtime` 分歧期） |
| `jit_deviation` | strict 一致但 JIT 跑完却错值/崩溃（违反“不崩溃不错值”） |
| `crash` / `hang` | strict 后端崩溃 / 超时 |

报告 `testing/artifacts/report.md` 含：配置 / oracle 说明 / 分类统计 / **按签名去重的独立问题**（同根因大量命中合并为一条）/ 命中用例表。
命中用例源码与各后端输出落盘于 `testing/artifacts/<类别>/seed_<seed>.{mini,json}`。

### 10.4 阶段二验收结果

- **基线健康**：默认剖面（statements=14, depth=2）下 40/40 种子全部 `agree` —— 生成器无误报。
- **已知法律样例**：8 个可独立运行样例中 6 个四后端一致（control_flow / error_handling 上 JIT 也跑完并一致），
  2 个分歧（即发现 1 / 2）。
- **新增发现 3**（报告已采）：RegisterVM 在程序规模增大时报 `Register IR lowering 失败`（编译期），
  而 interp/stackvm/stackvm-ir 正常运行。statements=10（100 行）正常、statements=15（276 行）触发；
  statements=30 时约 38% 种子命中（去重后为 **1 个独立问题**）。属 RegisterVM 32 虚拟寄存器的容量/寄存器压力限制。
  > 因此**默认剖面刻意保持小规模**（落在四后端均可编译的包络内），以便专注挖掘**语义**bug；
  > 需复现发现 3，以 `--statements 30` 跑批量即可。
- **本轮未发现新的运行期语义分歧**（安全子集上四后端运行语义一致）；唯一差异为 regvm 编译期容量。

---

## 11. 阶段三：Sanitizer 检测矩阵

用 sanitizer 构建的 runner 跑生成种子，把 ASan/UBSan 对**编译器/VM 自身**的
内存安全/UB 报错自动归类（每种类型单独计为一类 bug）。与差分投票正交：
程序输出可能四后端一致，但 sanitizer 仍可揭露隐藏的越界/UAF/UB。

### 11.1 构建器复用（无需新 CMake 选项）

项目已有 `MINILANG_SANITIZE` 选项与预设，runner 链接 `minilang_compile_options`（携带
 sanitizer flags），故**配置一个 sanitizer 构建目录即自动产出 sanitized runner**：

| 环境 | 预设 | Sanitizer | 说明 |
|------|------|-----------|------|
| **Linux/GCC/Clang（推荐）** | `linux-gcc-asan` | ASan + UBSan (`both`) | 全量，含 LSan/UBSan/抑制 |
| Windows/MSVC | `windows-msvc-asan` | 仅 ASan (`address`) | MSVC 无 UBSan/LSan（设计局限） |

```bash
# Linux（一键，推荐）
bash testing/run_sanitizers.sh 200 1      # N=200 种子，seed 起始 1

# 手动（任一平台）
cmake --preset linux-gcc-asan -DMINILANG_BUILD_TESTING_PIPELINE=ON
cmake --build out/build/linux-asan --target mini_diff_runner
python3 testing/diff_test.py --sanitized --n 200 --seed 1 --no-fail
```

### 11.2 `--sanitized` 开关

`driver.py` 与 `diff_test.py` 均支持 `--sanitized`：
- 自动定位 sanitizer 版 runner：`--san-runner` > `MINI_DIFF_RUNNER_SAN` > `out/build/{asan,linux-asan}` / `build_asan`。
- 为子进程注入 `ASAN_OPTIONS`/`UBSAN_OPTIONS`/`LSAN_OPTIONS`（`halt_on_error=0` 使尽量跑完；含抑制文件）。
- 解析 stderr 中的 sanitizer 报告 → `{tool, type[, detail]}`（tool ∈ asan/lsan/ubsan/tsan/msan）。
- 命中落盘于 `testing/artifacts/sanitizer/seed_<seed>.{mini,json}`（json 含各后端 `stderr` 与 sanitizer 列表）；
  报告 `report.md` 新增「Sanitizer 命中（按 tool:type 归并）」表。

### 11.3 Qt 环境的 ASan 已知问题与抑制

- **LSan 误报**：Qt 全局单例（`QGlobalStatic`/`QMetaType`/线程本地数据）按设计不释放，
  由 `testing/sanitizer/lsan.supp` 抑制（仅抑制栈顶落在 Qt 库的泄漏；涉及 minilang_core 的一律不抑制）。
- **UBSan vptr**：Qt 内部多态/函数指针模式由 `ubsan.supp` 抑制。
- **LD_PRELOAD**（Linux）：若宿主为非插桩可执行文件（如通过动态库间接启用 ASan），
  需 `LD_PRELOAD=$(clang -print-file-name=libclang_rt.asan-x86_64.so)`；本项目 runner 为
  直接链接 ASan 的可执行文件，**无需 LD_PRELOAD**。
- **MSVC ASan**：不支持 suppressions 文件与 LSan；driver 在 Windows 下仅设受支持的 `ASAN_OPTIONS`。
  运行需确保 `clang_rt.asan_dynamic-x86_64.dll` 在 PATH（随 MSVC 安装）。

### 11.4 阶段三验证状态（已真实跑通）

- **分类器已验证**：合成 ASan/LSan/UBSan 输出喂入 `parse_sanitizer_report` 均正确归类
  （heap-buffer-overflow / memory-leak / undefined-behavior），干净输出返回空。
- **端到端已验证（Windows/MSVC ASan）**：`windows-msvc-asan` 预设构建出 sanitized runner，
  `diff_test.py --sanitized --n 6` 实跑：成功识别并归类一个 **ASan `container-overflow`**
  （`RegisterVM::executeReturnImpl` @ `compiler/RegisterVMCalls.cpp:1044` 的 `ip = returnIp` 写），
  去重后为 1 个独立问题，落盘于 `sanitizer/` 并计入报告。
- **发现（已修复）**：该 `container-overflow` 经 `detect_container_overflow=0`
  复现验证为“容量内、size() 外”写——即 REG_RETURN 弹帧后，`ip`（引用 `frames_` 末尾帧
  的 ip 字段）成为指向已弹出 vector 死区的悬垂引用，写入属 UB（非 ASan 构建下因容量
  未回收而“碰巧运行”）。`frames_` 完全属 minilang_core（均 ASan 插桩），非混合插桩误报。
  **修复**（`compiler/RegisterVMCalls.cpp` + `RegisterVM.h`）：`executeReturnImpl` 不再接收
  `size_t& ip`（该引用绑定被调函数帧的 ip，弹帧后悬垂），删除 `ip = returnIp` 写入；
  REG_RETURN 后的 `notifyStep` 改读 `currentFrame().ip`（调用者帧，已由函数内部同步），
  并防御 `frames_` 空（main 帧返回）。**验证**：修复后 `--sanitized --n 6` 从 6 crash+6 命中
  变为 6/6 agree、0 sanitizer 命中；8 个 samples 差分与修复前逐字节一致，无新分歧。
- **UBSan**：MSVC 无 UBSan（平台限制）；--sanitized 的 UBSan 解析/抑制/环境已就绪，全量 ASan+UBSan
  在 **WSL/Linux + Clang/GCC**（`linux-gcc-asan`=both）一键跑：`bash testing/run_sanitizers.sh`。
- **已排除的环境坑**：(1) PATH 上 pip 的损坏 `ninja` 垫片→ 需显式 `-DCMAKE_MAKE_PROGRAM=<VS ninja>`；
  (2) `gui/I18n.h` 预存缺 `#include <QStringList>`（i18n 关闭分支用 `QStringList` 但仅 include `<QString>`），
  ASan 全量构建时暴露为 C2027，已补上该 include（构建性修复，非 Sanitizer 逻辑）。

---

## 12. 阶段五：用例自动缩减（`reduce.py` + `regression/`）

### 12.1 思路（对齐 C-Reduce）

`reduce.py` = 「变换 + 趣味性判定（interestingness）」的迭代不动点。
趣味性判定**复用阶段一 `driver`**：每次变换后重跑差分驱动，确认「同一类 bug」仍触发，
否则回退。判据可锁定：失败模式（`--mode` — 输出不一致/崩溃/超时/JIT 偏差）、
少数派后端（`--suspects`）、关键错误文本（`--match-error`），避免缩偏到另一个 bug。

变换 Pass（迭代至不动点）：`strip`（去注释/空行）→ `blocks`（清空 `{...}` 体）→
`lines`（行级 delta-debugging，多粒度贪婪删行）→ `tokens`（数字/字符串字面量简化）→
`rename`（标识符归一为短名）。

### 12.2 用法

```bash
# 缩减一个触发 bug 的程序（锁定“输出不一致 + interp 为少数派 + 错误含 getter”），
# 成功后拷入 regression/ 并登记 manifest（status=open）：
python testing/reduce.py reduce \
    --file testing/artifacts/disagreement/seed_functions_and_closures.mini \
    --mode disagreement --suspects interp --match-error getter \
    --name closure_block_named_fun --register --status open

# 也可用 JSON 配置（--config interesting.json），单后端崩溃：--oracle single --backend regvm --mode crash
```

`verify` 子命令（CTest 专用）：退出 `0` ≡ bug 已消失（判据不再成立），`1` ≡ bug 仍在。

### 12.3 CTest 回归（`pipeline.regression.*`，WILL_FAIL / 按修复状态）

`regression/manifest.json` 逐用例记录判据 + `status`，CMake 读取后注册：

- `status=open`（bug 未修）→ 标 **WILL_FAIL**：verify 退出 1（bug 在）→ CTest 判 **PASS**；
  一旦后端修复（四后端一致）→ verify 退出 0 → WILL_FAIL 转 **FAILED**，**自动提醒转正**。
- `status=fixed`（已修）→ 不标 WILL_FAIL：必须 verify 退出 0（防复发守卫，复发即 FAIL）。

修复后的转正：把 manifest 对应用例 `status` 改为 `fixed` 并重配置即可。

```bash
ctest --test-dir build_debug -R "pipeline.regression" --output-on-failure
```

### 12.4 验收结果（实测）

- 将 `seed_functions_and_closures.mini`（**153 非空行**）缩减至 **5 行 / 94 字节**（远低于 30 行目标）：
  ```
  var fns = [];
  for (var i = 0; i < 3; i = i + 1) {
      fun getter() {}
      fns.push(getter);
  }
  ```
  （连 `getter` 函数体与末尾 `print` 都自动剔除，比人工缩减更小）。
- 登记为 `pipeline.regression.closure_block_named_fun`（`known_bug` 标签 + WILL_FAIL），`ctest` 实测 **PASSED**。

---

## 13. 阶段六：静态分析接入（`static-analysis/`）

详见 [`static-analysis/README.md`](static-analysis/README.md)（安装方式 + `CLAZY_CHECKS` 推荐级别）。要点：

- **clang-tidy**：专用 `static-analysis/.clang-tidy` 聚焦 `bugprone-*` / `clang-analyzer-*` /
  `cppcoreguidelines-*` 三组（纯报告，不设 `WarningsAsErrors`，**不改根 `.clang-tidy`**）。
  `run_clang_tidy.py` 先**净化 MSVC 编译库**（剔 `/Yu` `/Fp<pch>` `/Zf` `/WX` 等，保留 `/FI`）再并行分析。
  > 需在 **VS DevShell** 内运行（clang cl 驱动模式靠 `INCLUDE` 定位 MSVC 系统头）。
- **clazy**（Qt 专用）：`run_clazy.py`；Windows 无成熟发行版，**推荐 Linux/WSL**（`apt install clazy`），
  `CLAZY_CHECKS` 推荐 **level1**。本机未装时输出如实的前置条件报告（不伪造 findings）。

报告：`static-analysis/reports/clang-tidy-report.md`、`static-analysis/reports/clazy-report.md`
（均含分级统计 + **Top 20 清单**：文件:行号 + 检查项 + 一句话解释）。仅报告，修复由维护者决定。

### 13.1 人工复核结论（2026-08-01，全量 268 TU / 2905 条）

对报告内**全部 high 级 + 语义风险类告警（约 50 条）逐条读源码复核**：
**未发现确认的实锤语义 bug，无需修改编译器代码**。判定明细：

- **误报（已读源码确认）**：`Value.h:952`（NaN-boxing 位模式欺骗路径敏感分析）、
  `Value.h:169/188`（memcpy 全覆盖）、`Value.h:229/1494`（if constexpr / 多类型同语义）、
  `VM.cpp`/`RegisterVMExec.cpp` 的 `ovr`/`arithOp`（输出参数契约 / default 兜底）、
  `lsp_core.cpp` `line/character` ×6（返回 true 必写）、`JITCodeGen.cpp:435`（Bug #21 有意识接受，
  注释明示）、`BuiltinMethods.cpp:167`（3 个调用点均保证 argCount↔args 一致）、
  `InterpreterCoroutine.cpp:59` guard 析构（`closeCapturedVariables` 键已存在 + Value 拷贝 noexcept，
  不抛异常）、GUI 层 5 条 high（Qt 父子所有权 / 构造期成员指针）、`WorkerManager` owning-memory
  （unique_ptr 自定义删除器标准写法）、branch-clone 系列（case 合并 / default 兜底 / 条件不同动作相同）。
- **设计选择（非 bug）**：`DebugTypes.h` 空 catch ×3（注释明示"解析失败视为无命中"）、
  owning-memory 1508 条（RefCounted/NaN-boxing 刻意设计）、`CodeEditor.cpp:2431` 三目同值（mask 区间等价，冗余无害）。
- **非项目代码**：EnumCastOutOfRange（MSVC 标准库头文件）。
- 理论风险（OOM 才触发，未修）：throwing-static-initialization ×30、exception-escape ×29。

> 结论：静态分析擅长找模式，本次命中均为误报/设计；**真正的语义 bug 依赖差分测试**
> （阶段二/五已实测发现块内命名函数分歧、RegisterVM 容量限制，且 RegisterVM `ip` 悬垂引用
> 已由 ASan container-overflow 检出并修复）。新报告生成后可复用本小节方法逐条复核。

---

## 14. CI 夜间部署（nightly.yml）

本流水线已部署到 `.github/workflows/nightly.yml`（每日 18:00 UTC + 手动 dispatch），
新增两个作业：

| 作业 | 内容 | 命中时 |
|------|------|--------|
| `pipeline-diff` | 构建 mini_diff_runner + 跑 `ctest -L pipeline` 回归守护（含 WILL_FAIL 自动转正）→ `diff_test.py --n 300` 生成式四后端差分（时间派生种子） | 作业失败 + 命中用例与报告归档 artifact（30 天） |
| `pipeline-sanitize` | `linux-gcc-asan` 预设（ASan+UBSan）构建 sanitized runner → `diff_test.py --sanitized --n 200` 扫生成程序 | 作业失败 + 用例与原始 stderr 归档 artifact |

后续人工流程：下载 artifact → `python testing/reduce.py reduce --file <case> --mode <类别> --register` 缩减并登记回归，即完成「发现 → 缩减 → 回归沉淀」闭环。
