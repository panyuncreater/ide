{
  "lastUpdate": 1791370507012,
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
          "id": "7da6aee2beddc8fd553da92a5dc8315548a1f500",
          "message": "fix(build): vptr 排除移入 MINILANG_SANITIZE=both 分支（组别名会重新启用先前的排除）\n\n00e5819 经 ci.yml 以全局 CMAKE_CXX_FLAGS 追加 -fno-sanitize=vptr，但 GCC 按命令行顺序处理 sanitize 标志——全局 flags 先于 minilang_compile_options INTERFACE 的 -fsanitize=address,undefined，组别名把 vptr 重新启用，linux-asan 首跑仍以 undefined typeinfo 断链（minilang_gui_smoke 链接失败实测）。修正：排除移入根 CMakeLists.txt both 分支的 INTERFACE 选项（组别名之后，次序生效）。该修改经维护者授权（只读范围一次性修改，随 ASan 门禁落地工作流）。同批含 PGO Pass 1 子集串行缓解（48cce7f）。",
          "timestamp": "2026-10-07T18:53:13+08:00",
          "tree_id": "c50ff604ab2e6c73209af558d9831593f74d1ce4",
          "url": "https://github.com/panyuncreater/ide/commit/7da6aee2beddc8fd553da92a5dc8315548a1f500"
        },
        "date": 1791370507011,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 510.213,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 28.8941,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 32.5094,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 20.4033,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 25.6093,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 17.8226,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 1.80706,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 1.54779,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 1.29203,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 61.6331,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 12.3908,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 10.7222,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 193.532,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 11.7647,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 12.3345,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 0.640828,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.053591,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.045889,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 0.965728,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 0.733517,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.010856,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 31.8055,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 4.86765,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 3.4296,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 1.87618,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.713777,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.010366,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 0.766306,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.040841,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.022463,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 4.62721,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}