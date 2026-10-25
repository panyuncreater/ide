{
  "lastUpdate": 1792899403473,
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
          "id": "a8a9431169ed80f2f2bade90aabae4d0cd195136",
          "message": "style: 全仓 clang-format 22.1.5 对齐\n\n- 对 CI 检查范围执行 clang-format -i，修正 66 个文件的既有格式违规\n- 纯格式变更；五目标构建 + ctest 4237/4237 通过\n- Code Style 作业恢复绿色\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-25T11:34:34+08:00",
          "tree_id": "1d4ef114a12bac8303a0f65de63b45098538c687",
          "url": "https://github.com/panyuncreater/ide/commit/a8a9431169ed80f2f2bade90aabae4d0cd195136"
        },
        "date": 1792899403472,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 917.337,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 48.5726,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 61.5133,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 37.4018,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 36.7998,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 32.7122,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.24844,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.4491,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.31764,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 108.406,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 20.8121,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 25.2724,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 335.415,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 19.6056,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 23.7143,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.1244,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.084017,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.082084,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.51591,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.14643,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.012815,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 52.9403,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 7.8539,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.92203,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 124.044,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 30.7328,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 29.3608,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 42.4313,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 33.1325,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 47.8952,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 4.03156,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 1.31231,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.019016,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.55384,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.062427,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.040816,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 9.716,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 36.3216,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 39.1555,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}