{
  "lastUpdate": 1791377952875,
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
          "id": "febc599b6bd6faf932ea6a2b325c220d9847fe45",
          "message": "fix(jit): IC helper 改经 ctx->backendPtr 解析后端实例（块缓存命中路径 UAF）\n\nLinux ASan 门禁（vptr 排除落地后首次完整测试）首抓真缺陷：TestJIT.BlockCacheHitWithStatefulProgram 报 stack-use-after-return——进程内块缓存（ADR-008 阶段 2）命中时，缓存生成代码携带创建实例烘焙的 JITBackend* this（ADR-006 地址表 C 类内嵌地址），jitMethodCall/jitMemberGetWithIC 经它访问 methodCallIC_/memberGetIC_ 即命中已析构实例；Windows/MSVC ASan 默认不开 use-after-return 检测故未暴露。修复：两 helper 改经 ctx->backendPtr（每执行实例在 initPerChunkState 设置，命中路径与全量编译共用）解析，烘焙参数保留为 ABI 占位并 [[maybe_unused]]——helper ABI 与 codegen 零改动。语义修正：命中实例重建的 IC 状态自此真正生效（此前被烘焙指针绕过）。另：PGO Pass 2 配置补 MINILANG_WERROR=OFF——/USEPROFILE 对 ctest 不训练的目标（test_harness 工具/plugin）无 profile 数据的告警被 release preset WERROR 默认 ON 抬成 C2220 → LNK1257（2026-07-24 WERROR 默认化以来未有机会暴露）。本地全量 4150/4150 通过。",
          "timestamp": "2026-10-07T20:57:05+08:00",
          "tree_id": "dd138d29a3fcc64583c3617ed87ce48296345f74",
          "url": "https://github.com/panyuncreater/ide/commit/febc599b6bd6faf932ea6a2b325c220d9847fe45"
        },
        "date": 1791377952874,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "Interpreter::fibonacci",
            "value": 912.138,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::fibonacci",
            "value": 56.8911,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::fibonacci",
            "value": 58.951,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::large_loop",
            "value": 41.5254,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::large_loop",
            "value": 40.3207,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::large_loop",
            "value": 30.5575,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::string_concat",
            "value": 3.34492,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::string_concat",
            "value": 2.77868,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::string_concat",
            "value": 2.52607,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::array_alloc",
            "value": 107.585,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::array_alloc",
            "value": 22.1934,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::array_alloc",
            "value": 20.3854,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::tak",
            "value": 332.332,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::tak",
            "value": 22.3819,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::tak",
            "value": 22.9709,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::ackermann",
            "value": 1.17418,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::ackermann",
            "value": 0.114975,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::ackermann",
            "value": 0.079358,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::bubble_sort",
            "value": 1.77491,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::bubble_sort",
            "value": 1.3008,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::bubble_sort",
            "value": 0.018595,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "Interpreter::closure_counter",
            "value": 55.4135,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "StackVM::closure_counter",
            "value": 8.86659,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "RegisterVM::closure_counter",
            "value": 6.79269,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::fibonacci",
            "value": 3.78827,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::large_loop",
            "value": 1.39259,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::string_concat",
            "value": 0.019767,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::tak",
            "value": 1.60071,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::ackermann",
            "value": 0.067376,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::bubble_sort",
            "value": 0.040135,
            "unit": "ms",
            "extra": "[object Object]"
          },
          {
            "name": "JIT::closure_counter",
            "value": 9.61891,
            "unit": "ms",
            "extra": "[object Object]"
          }
        ]
      }
    ]
  }
}