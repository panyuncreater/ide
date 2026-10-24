{
  "lastUpdate": 1792812255260,
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
      },
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
        "date": 1792311849901,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 881.07,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 57.7883,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 59.2008,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 42.0769,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 41.1667,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 30.517,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.40973,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.76371,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.28716,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 108.366,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 22.7013,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 20.6894,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 333.455,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 22.6304,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 24.1473,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.99779,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.14901,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.147056,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 3.36788,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.60625,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.013495,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 60.446,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 8.85148,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.75666,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 4.00368,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 2.26502,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.020598,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.54141,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.06415,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.063939,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 9.59271,
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
          "id": "ee2ae3622d55c82795cb5409ed83160e169503e4",
          "message": "perf(backlog): 清偿 §2 的 17 项优化候选\n\n- 解释器（OPT-01/02/03）：单节点 VarRef 原位变异快路径、debugger 原子短路、\n  字符串字面量原子缓存；MethodCall/FieldAccess 基准 -12.4%/-29.7%\n- VM（OPT-05/06/07）：方法返回接收者整体写回、帧字段单次遍历、\n  RegisterVM 名称版全局缓存与版本失效\n- IR/字节码（OPT-08/09/10）：结构化值编号键、行列元数据 4B/字节\n  （.mbc/.mrc/.minic 格式 v4）；OPT-09 核实 HEAD 已具备收敛退出\n- JIT（OPT-13/15/16/17）：接收者整体替换、透明哈希、init 缓存、\n  currentFrame 内联/pushNulls/帧名稳定指针\n- 前端（OPT-12/14）：formatter 单输出缓冲、去除无效 StringIntern\n- GUI（OPT-11）：六位硬编码色迁移 TeachingTheme\n- 编译器（OPT-18）：GT/GTE/LTE int 特化；B4/B5 入 rejected R-19/R-20\n\n同时携带工作树既有缺陷清偿（B-001/002/003/006/007/008）与全部文档同步；\n全量 ctest 4221/4221，五目标 /W4+/WX 零警告。\n\nKnowledge-Impact: changelog",
          "timestamp": "2026-10-24T09:25:06+08:00",
          "tree_id": "3820b7db39b76d4d4667a9f03ae327fd8dee4e52",
          "url": "https://github.com/panyuncreater/ide/commit/ee2ae3622d55c82795cb5409ed83160e169503e4"
        },
        "date": 1792805239769,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 794.04,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 47.3996,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 57.4648,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 40.0508,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 37.0395,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 31.6192,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.09448,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.43486,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.22261,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 98.8755,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 20.3338,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 19.7098,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 296.192,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 18.7642,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 23.1587,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.83889,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.235906,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.291831,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.61878,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.33418,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.015343,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 46.7795,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 7.63173,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.51572,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 111.661,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 26.7755,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 25.3741,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 40.9687,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 31.0827,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 42.0703,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 4.05182,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 1.40761,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.016925,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.46157,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.058769,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.040281,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 11.1155,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 34.8362,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 42.3842,
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
          "id": "47414fe7577d51de6a0b82a1a56cc901af9531f8",
          "message": "ci(qfluentkit): 补丁应用改为幂等守卫\n\n- fork 子模块已携带补丁；所有 workflow 在 git apply 前先 --reverse --check\n- 已应用则跳过，未应用则按原路径应用\n- 覆盖 ci/nightly/release/benchmark-tracking 共 19 处\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-24T10:41:36+08:00",
          "tree_id": "098eff691d9ba6173d4062a58b0cf3b9fca5db8e",
          "url": "https://github.com/panyuncreater/ide/commit/47414fe7577d51de6a0b82a1a56cc901af9531f8"
        },
        "date": 1792809808680,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 502.621,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 23.0452,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 30.0899,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 19.9894,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 20.8054,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 19.4884,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 1.82599,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 1.42708,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 1.41177,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 62.1813,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 10.7074,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 11.0276,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 189.867,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 9.84139,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 12.4779,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 0.673387,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.054582,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.048393,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 0.797072,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 0.647769,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.016996,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 29.8657,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 4.0434,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 3.67763,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 69.0864,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 14.8429,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 14.1513,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 22.247,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 16.1361,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 24.3029,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 2.13041,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.718294,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.011677,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 0.843342,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.029524,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.031457,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 4.60569,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 18.6482,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 26.528,
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
          "id": "67a07b8ae72aa2691efefeac2a5b194e0c9e26cd",
          "message": "ci(qfluentkit): Dockerfile 补丁步骤幂等化\n\n- fork 子模块已携带同一补丁；patch 前用 dry-run --forward 检查\n- 已应用则跳过，修复 Docker Build 在反转补丁提示后退出 1\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-24T10:43:03+08:00",
          "tree_id": "c2f28a18255d182a4332872aa034f690ba5d99d0",
          "url": "https://github.com/panyuncreater/ide/commit/67a07b8ae72aa2691efefeac2a5b194e0c9e26cd"
        },
        "date": 1792809890576,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 487.423,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 22.8829,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 29.6714,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 18.1573,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 20.4407,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 18.2458,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 1.69241,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 1.27901,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 1.25871,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 56.548,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 9.90758,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 11.8804,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 203.571,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 17.0197,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 16.1146,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 0.774288,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.054042,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.056425,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.07433,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 0.823811,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.009414,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 30.7214,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 3.72451,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 3.47033,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 65.941,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 14.0765,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 14.3834,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 20.8974,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 16.0995,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 22.4933,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 1.97768,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 0.682689,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.010396,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 0.810031,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.027231,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.023776,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 4.42208,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 17.4052,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 24.2216,
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
          "id": "3338355c7333eb1f6fddd9ea501f17a50dbf9638",
          "message": "fix(gui,i18n): 补全 StepExplainer 新 OpCode 分支\n\n- 增加 OP_DROP_PENDING_JUMPS / OP_GT_INT_SPEC / OP_GTE_INT_SPEC / OP_LTE_INT_SPEC\n- 修复 GCC/Clang -Werror=switch 导致的 Linux/macOS/Docker 构建失败\n- 同步新增说明字符串，i18n 保持 787/787\n\nKnowledge-Impact: none",
          "timestamp": "2026-10-24T11:22:14+08:00",
          "tree_id": "13b5332e92f04ca52dc8e97e4b7330dabe750592",
          "url": "https://github.com/panyuncreater/ide/commit/3338355c7333eb1f6fddd9ea501f17a50dbf9638"
        },
        "date": 1792812255259,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 893.025,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 50.0487,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 58.4436,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 39.5706,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 34.2897,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 33.9944,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.49628,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.43534,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.31758,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 106.416,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 21.4654,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 20.5558,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 330.2,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 19.2215,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 22.4433,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.14849,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.091021,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.079359,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.48819,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.13352,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.017763,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 50.9657,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 7.8293,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.60973,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::method_call",
            "value": 122.371,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::method_call",
            "value": 27.9694,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::method_call",
            "value": 28.3828,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::field_access",
            "value": 43.7867,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::field_access",
            "value": 29.5296,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::field_access",
            "value": 44.8545,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 4.06967,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 1.32746,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.020879,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.6114,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.059071,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.051286,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 9.75334,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::method_call",
            "value": 33.6745,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::field_access",
            "value": 40.733,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}