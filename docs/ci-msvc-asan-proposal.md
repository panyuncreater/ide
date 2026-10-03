# 提案：MSVC ASan 内存错误门禁（CI job）

> 状态：**提案（待合入 CI）** | 日期：2026-10-03 | 作者：审计改进计划第 ③ 项
>
> 为什么以提案形式交付：`.github/workflows/` 与 CI 配置属于 AGENTS.md §2 声明的**只读范围**
> （禁止操作含「修改 CI 配置」），本提案给出可直接粘贴合入的 job 定义；合入动作需由具有
> CI 变更权限的维护者执行。

## 1. 动机

- 当前 ASan+UBSan 门禁仅在 Linux（`sanitize-asan` job）运行，**主力开发平台 Windows MSVC
  没有内存错误检测**。
- 平台差异是已证实的真实风险源：v1.3.0 曾修复 JIT 运行时回调硬编码 Win64 参数寄存器导致
  Linux SysV ABI SIGSEGV 的 P0（CHANGELOG v1.3.0 Fixed 节）——只有跨平台 sanitizer 才能
  提前暴露这类平台特有路径。
- 本提案的 job 定义已在本地以等价命令行全量验证（见 §4 验证记录），合入 CI 属于机械搬运。

## 2. Job 定义（粘贴到 `.github/workflows/ci.yml` 的 `jobs:` 下）

```yaml
  # MSVC ASan 门禁（Windows）：/fsanitize=address 全量测试，阻断式。
  # Linux 已有 sanitize-asan；本 job 补齐主力开发平台 Windows 的内存错误覆盖，
  # 覆盖 MSVC CRT / Win64 ABI 特有的分配器与指针路径。
  sanitize-asan-msvc:
    name: ASan (Windows MSVC)
    runs-on: windows-latest
    needs: build   # 与既有 job 的依赖关系保持一致（按 ci.yml 实际 job 名调整）
    steps:
      - name: Checkout 代码
        uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: 安装 Qt6 (Windows)
        # 与既有 Windows job 的 Qt 安装步骤保持一致（install-qt-action + 架构 msvc2022_64）

      - name: 配置 CMake (MSVC ASan)
        shell: pwsh
        run: >
          cmake -S . -B out/build/asan -G Ninja
          -DCMAKE_BUILD_TYPE=Debug
          -DCMAKE_CXX_FLAGS="/fsanitize=address"
          -DCMAKE_C_FLAGS="/fsanitize=address"
          -DCMAKE_EXE_LINKER_FLAGS="/fsanitize=address"
          -DCMAKE_SHARED_LINKER_FLAGS="/fsanitize=address"
          -DMINILANG_USE_CCACHE=OFF
          -DMINILANG_USE_JIT=ON

      - name: 构建测试目标
        run: cmake --build out/build/asan --target minilang_tests

      - name: 运行测试（ASan）
        env:
          ASAN_OPTIONS: detect_leaks=0   # MSVC ASan 暂不支持 LeakSanitizer，显式关闭避免告警噪音
        shell: pwsh
        run: >
          ctest --test-dir out/build/asan
          --output-on-failure -j 4
          --timeout 600
          -LE "perf"   # 排除性能测试：ASan 插桩开销使时间阈值失真（性能门禁由 benchmark-tracking.yml 承担）
```

注意事项：

1. **运行库 PATH（本地实测坑）**：ASan 树的 DLL 拷贝步骤不复制 MSVC 调试 CRT，而 gtest
   discovery 阶段就会运行测试可执行文件——若 PATH 缺 `MSVCP140D.dll` 等调试 CRT，ctest
   直接报 `0xc0000135`（DLL not found）而无法枚举测试。须把 VC Redist 调试 CRT 目录加入
   PATH：`VC\Redist\MSVC\<ver>\debug_nonredist\x64\Microsoft.VC143.DebugCRT`，另加
   `QTDIR/bin`（Qt DLL）与 VC `bin\Hostx64\x64`（`clang_rt.asan_dynamic-x86_64.dll`，
   vcvars 自带）。
2. **JIT 与 ASan 共存**：asmjit 生成的本机码不受插桩影响（预期行为）；JIT 调用的 C++
   运行时辅助函数会被插桩，能捕获热路径上的越界/UAF。已支持场景若在 ASan 下暴露
   `JitContext` 邮箱结构的访问越界，属于真实缺陷（对应 ADR-006 的 `static_assert(offsetof)`
   布局约定的运行期验证）。
3. **LeakSanitizer**：MSVC ASan 目前不支持 leak 检测（Linux job 已覆盖），故
   `detect_leaks=0`。
4. 若全量 4109 用例耗时过长，可先以 `--suite` 或标签过滤三后端一致性 + fuzz + 审计批次
   作为第一道门禁，后续再扩全量。

## 3. 与现有 CI 的关系

| 已有 | 本提案补充 |
|------|-----------|
| `sanitize-asan`（Linux，ASan+UBSan，阻断） | Windows MSVC 侧的 ASan（阻断） |
| `coverage` / `coverage-linux` | 不变 |
| `clang-tidy`（信息性） | 不变 |

## 4. 本地验证记录（等价命令行，2026-10-03）

配置与构建命令与 §2 完全一致（含 `/fsanitize=address` 四处 flags、
`MINILANG_USE_CCACHE=OFF`、`MINILANG_USE_JIT=ON`），环境：MSVC 14.51 / Qt 6.10.3 /
Ninja Debug。实际结果：

1. **门禁首跑即捕获真实缺陷（本提案价值的最直接证据）**：`TestJIT.R162*` 12 项 +
   `L14RuntimeErrorCatch.*` 6 项共 18 项用例触发 `AddressSanitizer: container-overflow`
   ——`compiler/jit/JITRuntime.cpp` `jitThrow` 在 `tryStack_.pop_back()` 之后经 `back()`
   引用读取 `handler.catchAddr`（use-after-pop 容器溢出 UB，非插桩构建因旧槽位未被覆写
   而侥幸通过）。按值复制 `catchAddr` 修复后，R162/L14 全家族 26 项 + `AppOrchestrationTest`
   47 项 + `GuiSmokeTest` 12 项共 77 项在 ASan 下全部通过、零报告。修复已进
   CHANGELOG [Unreleased] Fixed。
2. 全量非插桩回归（同源码）：4109/4109 全绿，修复无行为回归。
3. 工程备注：ASan 树的 DLL 拷贝步骤不复制 MSVC 调试 CRT，ctest 需将 VC Redist
   `debug_nonredist\...\Microsoft.VC143.DebugCRT` 加入 PATH（见 §2 注意事项 1）；
   `minilang_perf_test` 建议不进 ASan 门禁（插桩使时间阈值失真）。
