{
  "lastUpdate": 1791714248077,
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
          "id": "e937640be700f942fc6f4224ddc75c8adc6cb9bc",
          "message": "fix(ci): PGO Pass 1 补 perf 族训练（minilang_perf_test 无 pgd 致 Pass 2 LNK1266）\n\nPass 1 训练切片（-I 1,400）未覆盖注册表末尾的 PerfBenchmark 族——minilang_perf_test 的 .pgd 只在其二进制运行时创建，Pass 2 /USEPROFILE 链接该目标时 LNK1266 硬错误（其余无 pgd 目标在 WERROR=OFF 下为可容忍告警，仅 perf_test 硬失败，2026-10-07 第七轮实测）。Pass 1 追加 -R PerfBenchmark 串行训练：perf 测试只输出 JSON 无 pass/fail 断言，插桩减速不影响通过，且其热循环（三后端解释循环）恰为 PGO 核心优化目标。",
          "timestamp": "2026-10-07T14:19:39Z",
          "url": "https://github.com/panyuncreater/ide/commit/e937640be700f942fc6f4224ddc75c8adc6cb9bc"
        },
        "date": 1791714248077,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 513.893,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 28.3606,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 34.629,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 20.8862,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 25.7386,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 17.6074,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 1.81242,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 1.50056,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 1.31788,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 61.0064,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 11.7423,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 11.0838,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 191.64,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 11.734,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 12.3038,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 0.655861,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.054463,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.044797,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 0.965779,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 0.744935,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.009094,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 30.1784,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 4.66753,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 3.38412,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 1.88817,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.70811,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.010246,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 0.766719,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.034902,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.022253,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 4.72304,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}