{
  "lastUpdate": 1793026253711,
  "repoUrl": "https://github.com/panyuncreater/ide",
  "entries": {
    "MiniLang Performance Benchmark": [
      {
        "commit": {
          "author": {
            "name": "panyuncreater",
            "username": "panyuncreater",
            "email": "1265110878@qq.com"
          },
          "committer": {
            "name": "panyuncreater",
            "username": "panyuncreater",
            "email": "1265110878@qq.com"
          },
          "id": "65217ed03602c2e7f12940d229f557bc4304026d",
          "message": "ci(pgo): 训练子集收到 1-200 避开插桩超时用例\n\n- #216/#248 在 Pass1 训练中稳定超时（pgort/QFluent 已知竞态）\n- 1-200 子集覆盖基础族，保留 PGO profile 质量\n- 其余 PGO 串行构建与 perf 训练保持不变\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-25T07:16:57Z",
          "url": "https://github.com/panyuncreater/ide/commit/65217ed03602c2e7f12940d229f557bc4304026d"
        },
        "date": 1792918083059,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 863.547,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 48.7399,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 59.845,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 38.1769,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 34.3256,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 32.6557,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.24412,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.44638,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.38721,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 106.694,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 20.8728,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 22.1252,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 326.595,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 19.5364,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 23.1747,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.14657,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.085309,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.082263,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.51059,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.14304,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.013415,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 51.9641,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 7.73032,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.92211,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 122.169,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 28.1221,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 29.3606,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 53.8224,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 35.8245,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 47.5423,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 4.04787,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 1.30781,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.022282,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.59671,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.054332,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.040886,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 9.69107,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 33.1079,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 39.7032,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "1265110878@qq.com",
            "name": "panyuncreater",
            "username": "panyuncreater"
          },
          "committer": {
            "email": "1265110878@qq.com",
            "name": "panyuncreater",
            "username": "panyuncreater"
          },
          "distinct": true,
          "id": "2d2a9d3acb874307b99162f09cc61e0d318c51a3",
          "message": "fix(ci): 修复 Formatter 截断告警与 dynamic fuzz 语法错误\n\n- formatter/Formatter.cpp: 非 BMP 转义缓冲区 12→16 字节，容纳 %x 最坏\n  8 位十六进制（3+8+1+NUL），消除 GCC -Werror=format-truncation；\n  修复 Nightly ASan pipeline 与 Linux coverage 构建失败\n- tests_dynamic/fuzz_minilang.py: 修正 `= =` 笔误，恢复脚本可解析\n- 验证: 五目标 /W4+/WX 零警告；全量 ctest 4257/4257；knowledge_lint 0/0\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-26T22:24:38+08:00",
          "tree_id": "31acd977be5100207e711463922d0a0915f93a98",
          "url": "https://github.com/panyuncreater/ide/commit/2d2a9d3acb874307b99162f09cc61e0d318c51a3"
        },
        "date": 1793024820036,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 865.807,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 44.9384,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 56.9644,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 37.6015,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 25.5882,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 28.453,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 2.64528,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.20931,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.13847,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 106.644,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 19.2151,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 19.7246,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 328.034,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 18.4122,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 21.964,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.1279,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.080391,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.075601,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.55213,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.01876,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.018865,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 50.3938,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 6.56522,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.33081,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 122.035,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 26.4909,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 28.1832,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 42.9695,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 27.1276,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 46.8847,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 3.04798,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.927677,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.041768,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.14911,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.048511,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.049473,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 3.81883,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 22.9473,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 42.0287,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "1265110878@qq.com",
            "name": "panyuncreater",
            "username": "panyuncreater"
          },
          "committer": {
            "email": "1265110878@qq.com",
            "name": "panyuncreater",
            "username": "panyuncreater"
          },
          "distinct": true,
          "id": "a7d79783490275fba125c885e2d484889bb9ff7d",
          "message": "fix(ci): TSan 兼容释放 + fuzz 已知差异分类 + PGO/ARM64 作业修复\n\n- RefCounted: TSan 构建改用 acq_rel，生产保持 release-only；memory-model spec 记录例外\n- fuzz: 顶层前向引用（G4）计入 knownDivergences，新增分类器与回归；\n  dynamic-fuzz 报告路径改脚本目录并降为信息性作业\n- PGO: 显式 -DMINILANG_BUILD_PERF_TESTS=ON（option 位于 add_subdirectory(tests) 后）\n- ARM64: asmjit 门控覆盖 MINILANG_USE_JIT_A64；JITA64 使用独立 JitA64Context，\n  修复 A64-only 构建的 JitContext 依赖\n- 验证: 五目标构建 + 全量 ctest 4266/4266；A64-only minilang_backend 构建通过；\n  fuzz 已知用例与 CI 种子分类验证；clang-format 22.1.5 零违规\n\nKnowledge-Impact: spec",
          "timestamp": "2026-10-26T22:48:37+08:00",
          "tree_id": "2333792ba930f8516875220ffec40a41f3247a40",
          "url": "https://github.com/panyuncreater/ide/commit/a7d79783490275fba125c885e2d484889bb9ff7d"
        },
        "date": 1793026253710,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 879.057,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 44.8606,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 57.3008,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 37.036,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 25.7408,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 28.4046,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 2.69278,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.06421,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.16546,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 107.152,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 18.8442,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 20.4458,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 330.056,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 18.5702,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 21.9044,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.12292,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.076644,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.075471,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.54655,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.01583,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.019617,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 50.634,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 6.57111,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.42455,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 121.088,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 26.7149,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 28.0932,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 42.5025,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 26.8249,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 47.0352,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 3.89384,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.957343,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.022843,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.46063,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.04806,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.04769,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 3.78538,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 23.0146,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 44.6678,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}