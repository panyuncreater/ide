{
  "lastUpdate": 1792909144412,
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
          "id": "c9538fca4688fab54544c6cb3e4e58d22f1bacbd",
          "message": "ci(pgo): 测试目标串行构建避免 Qt 部署竞态\n\n- PGO Pass1/2 主目标并行、四个测试目标逐个构建\n- 规避 windeployqt 同时写 tests/Qt6Core.dll 的 Destination file exists\n- 保留 Pass2 的 PerfBenchmark 性能验证过滤\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-25T14:17:02+08:00",
          "tree_id": "a95260b9811bfc4249e8ceb6c6495482f70bea34",
          "url": "https://github.com/panyuncreater/ide/commit/c9538fca4688fab54544c6cb3e4e58d22f1bacbd"
        },
        "date": 1792909144411,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 507.392,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 25.8944,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 33.8802,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 19.6033,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 20.0419,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 18.4691,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 1.73733,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 1.35341,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 1.333,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 62.2383,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 12.1407,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 12.3229,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 193.339,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 10.1093,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 13.6343,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 0.694208,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.110474,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.057585,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 0.910167,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 0.680177,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.023195,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 30.8968,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 4.22386,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 3.9815,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 70.818,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 14.9998,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 15.1817,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 22.3868,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 16.345,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 24.314,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 2.11261,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.73673,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.011497,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 0.836718,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.042343,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.028682,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 5.34976,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 19.8254,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 25.8543,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}