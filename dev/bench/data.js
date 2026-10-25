{
  "lastUpdate": 1792918083060,
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
      }
    ]
  }
}