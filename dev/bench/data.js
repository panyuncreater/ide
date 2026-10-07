{
  "lastUpdate": 1791382913173,
  "repoUrl": "https://github.com/panyuncreater/ide",
  "entries": {
    "MiniLang Performance Benchmark": [
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
          "id": "e937640be700f942fc6f4224ddc75c8adc6cb9bc",
          "message": "fix(ci): PGO Pass 1 补 perf 族训练（minilang_perf_test 无 pgd 致 Pass 2 LNK1266）\n\nPass 1 训练切片（-I 1,400）未覆盖注册表末尾的 PerfBenchmark 族——minilang_perf_test 的 .pgd 只在其二进制运行时创建，Pass 2 /USEPROFILE 链接该目标时 LNK1266 硬错误（其余无 pgd 目标在 WERROR=OFF 下为可容忍告警，仅 perf_test 硬失败，2026-10-07 第七轮实测）。Pass 1 追加 -R PerfBenchmark 串行训练：perf 测试只输出 JSON 无 pass/fail 断言，插桩减速不影响通过，且其热循环（三后端解释循环）恰为 PGO 核心优化目标。",
          "timestamp": "2026-10-07T22:19:39+08:00",
          "tree_id": "22e6d9ab604bbc4daf550a6ea13951dc47ca97eb",
          "url": "https://github.com/panyuncreater/ide/commit/e937640be700f942fc6f4224ddc75c8adc6cb9bc"
        },
        "date": 1791382913172,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 888.279,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 58.8712,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 59.403,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 48.7992,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 40.8656,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 31.1508,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.40812,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.75777,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.27046,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 109.018,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 22.425,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 20.8364,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 334.594,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 22.3452,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 23.1015,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.19378,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.105376,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.0807,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.81879,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.30553,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.017302,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 52.5667,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 8.80881,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.76522,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 3.81995,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 1.39653,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.023143,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.57078,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.068338,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.05877,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 9.69806,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}