// ============================================================
// IROptReplayLibrary.cpp — IR 逐步优化回放场景库实现（P3-20 拆分）
// ------------------------------------------------------------
// 从 gui/IRTransformPanel.cpp 拆分而来，与 IRTransformLibrary.cpp /
// BugHuntLibrary.cpp / MemoryModelLibrary.cpp 拆分模式一致：将纯静态
// 教学数据独立成库，让面板文件聚焦于 UI 逻辑。
//
// 本文件提供逐步优化回放场景库（第 4 子页），每个场景包含 3 个
// IROptStepRecord，逐步回放一个优化 pass 的执行轨迹：
//   - replay-const-fold：常量折叠回放（3 步骤）
//   - replay-dce：死代码消除回放（3 步骤）
//   - replay-copy-prop：复制传播回放（3 步骤）
//   - replay-cse：公共子表达式消除回放（3 步骤）
//   - replay-loop-unroll：循环展开回放（3 步骤）
//
// irSnapshot 采用与 IRToString 一致的人类可读格式（含 "function" 关键字），
// decisions 字段含具体指令级别的修改/删除原因，便于学习者理解 pass 内部行为。
// ============================================================

#include "gui/IRTransformPanel.h" // IROptStepRecord 结构定义

#include <utility> // std::pair
#include <vector>

/// 返回逐步优化回放场景库（静态单例）：每个场景含 3 个 IROptStepRecord，
/// 逐步回放一个优化 pass（常量折叠 / DCE / 复制传播 / CSE / 循环展开）的执行轨迹，
/// 含每轮 IR 快照与指令级修改/删除决策，供回放页展示。
const std::vector<std::pair<std::string, std::vector<IROptStepRecord>>>& IROptReplayLibrary::replayScenarios() {
    static const std::vector<std::pair<std::string, std::vector<IROptStepRecord>>> kScenarios = {
        // ---- 1. 常量折叠回放（3 步骤）----
        {"replay-const-fold",
         {{"✨ 常量折叠",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)\n"
           "    v1 = LOAD_CONST #1 (2)\n"
           "    v2 = ADD v0, v1              # 1 + 2 → 3 (本次折叠)\n"
           "    v3 = LOAD_CONST #2 (3)\n"
           "    v4 = LOAD_CONST #3 (4)\n"
           "    v5 = MUL v3, v4              # 待下轮折叠\n"
           "    v6 = STORE_LOCAL slot=0, v5   # x = (1+2) * (3*4)\n"
           "    end block\n"
           "}\n",
           {"v2 = ADD v0, v1 → LOAD_CONST #2 (3) — 折叠原因：v0=LOAD_CONST #0 (1), v1=LOAD_CONST #1 (2) 均为常量加载",
            "保留 v3 = LOAD_CONST #3 (4), v4 = LOAD_CONST #4 (4) — MUL "
            "操作数均为常量但下一轮再折叠（保守策略，避免一次折叠破坏数据流分析）",
            "v0/v1 暂不删除 — DCE pass 会在后续步骤中清理无引用的纯计算指令"},
           8,
           1},
          {"✨ 常量折叠",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)\n"
           "    v1 = LOAD_CONST #1 (2)\n"
           "    v2 = LOAD_CONST #2 (3)        # 已折叠\n"
           "    v3 = LOAD_CONST #3 (3)\n"
           "    v4 = LOAD_CONST #4 (4)\n"
           "    v5 = MUL v3, v4 → LOAD_CONST #5 (12)  # 3 * 4 → 12 (本次折叠)\n"
           "    v6 = STORE_LOCAL slot=0, v5   # x = 12\n"
           "    end block\n"
           "}\n",
           {"v5 = MUL v3, v4 → LOAD_CONST #5 (12) — 折叠原因：v3=LOAD_CONST #3 (3), v4=LOAD_CONST #4 (4) 均为常量加载",
            "新增常量 #5 (12) 加入常量池（重复时复用索引）", "v3/v4 暂不删除 — 等待 DCE pass 清理"},
           8,
           1},
          {"✨ 常量折叠",
           "Round 3 (收敛)",
           "function main {\n"
           "  block L0:\n"
           "    v2 = LOAD_CONST #2 (3)        # 已折叠\n"
           "    v5 = LOAD_CONST #5 (12)       # 已折叠\n"
           "    v6 = STORE_LOCAL slot=0, v5   # x = 12\n"
           "    end block\n"
           "}\n",
           {"本 round 未发生折叠 — IR 已收敛（无更多可折叠的算术指令）",
            "下一阶段：DCE pass 将清理 v0/v1/v3/v4 等无引用的 LOAD_CONST"},
           3,
           0}}},
        // ---- 2. 死代码消除回放（3 步骤）----
        {"replay-dce",
         {{"🧹 DCE",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)       # 待分析\n"
           "    v1 = LOAD_CONST #1 (2)       # 待分析\n"
           "    v2 = LOAD_CONST #2 (\"hello\")\n"
           "    v3 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
           "    CALL_PRINT v2                 # 副作用：打印\n"
           "    end block\n"
           "}\n",
           {"扫描所有指令的操作数 → 收集被引用的 vreg 集合 used = {v0 (by STORE_LOCAL), v2 (by CALL_PRINT)}",
            "标记 LOAD_CONST #1 (2) → v1：dest v1 未出现在任何指令的操作数中，且无副作用 → 候选删除",
            "保守保留 v0/v2/v3/CALL_PRINT — STORE_LOCAL 写入内存（可能被闭包/upvalue 捕获），CALL_PRINT 有 I/O 副作用"},
           5,
           1},
          {"🧹 DCE",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)       # 重新分析\n"
           "    v2 = LOAD_CONST #2 (\"hello\")\n"
           "    v3 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
           "    CALL_PRINT v2                 # 副作用：打印\n"
           "    end block\n"
           "}\n",
           {"删除 v1 = LOAD_CONST #1 (2) — 原因：dest v1 从未被后续指令引用，纯计算无副作用",
            "重新扫描：used = {v0 (by STORE_LOCAL), v2 (by CALL_PRINT)}",
            "发现 v3 = STORE_LOCAL slot=0, v0 — STORE_LOCAL 写入局部变量槽位，可能被后续代码或闭包捕获，保守保留"},
           4,
           1},
          {"🧹 DCE",
           "Round 3 (报告)",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)       # 保留\n"
           "    v2 = LOAD_CONST #2 (\"hello\")\n"
           "    v3 = STORE_LOCAL slot=0, v0   # 保留\n"
           "    CALL_PRINT v2                 # 保留\n"
           "    end block\n"
           "}\n",
           {"报告：本 round 共删除 1 条指令（v1 = LOAD_CONST #1 (2)）", "本轮 DCE 收敛 — 未发现新的可删除指令",
            "保留指令原因：v0 被 STORE_LOCAL 引用、v2 被 CALL_PRINT 引用、STORE_LOCAL 有写副作用、CALL_PRINT "
            "有打印副作用"},
           4,
           0}}},
        // ---- 3. 复制传播回放（3 步骤）----
        {"replay-copy-prop",
         {{"📋 复制传播",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = MOVE v0              # b = a (复制模式识别)\n"
           "    v2 = LOAD_CONST #0 (1)\n"
           "    v3 = ADD v1, v2           # b + 1 (待替换)\n"
           "    v4 = STORE_LOCAL slot=1, v3   # c = b + 1\n"
           "    end block\n"
           "}\n",
           {"扫描指令识别 MOVE 模式 → 建立 copy 链：v1 = MOVE v0 (即 v1 等价于 v0)",
            "记录 v1 的等价源为 v0 — 后续指令引用 v1 处可替换为 v0", "暂不修改 — 等待下一 round 执行替换"},
           5,
           1},
          {"📋 复制传播",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = MOVE v0              # b = a (待删除)\n"
           "    v2 = LOAD_CONST #0 (1)\n"
           "    v3 = ADD v0, v2           # a + 1 (v1 → v0 已替换)\n"
           "    v4 = STORE_LOCAL slot=1, v3   # c = a + 1\n"
           "    end block\n"
           "}\n",
           {"替换 v3 = ADD v1, v2 中操作数 v1 → v0 — 原因：v1 = MOVE v0 等价复制关系",
            "标记 v1 = MOVE v0 为待删除 — 替换后 v1 不再被任何指令引用",
            "本 round 未跨 STORE_LOCAL slot=0 传播 — STORE_LOCAL 可能修改 a 的值，传播链在此终止"},
           5,
           1},
          {"📋 复制传播",
           "Round 3 (DCE 二次清理)",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v2 = LOAD_CONST #0 (1)\n"
           "    v3 = ADD v0, v2           # a + 1\n"
           "    v4 = STORE_LOCAL slot=1, v3   # c = a + 1\n"
           "    end block\n"
           "}\n",
           {"DCE 二次清理：删除 v1 = MOVE v0 — 原因：替换后 v1 不再被任何指令引用，纯复制指令无副作用",
            "指令数从 5 减少到 4 — 复制传播 + DCE 联合优化效果", "本 round 收敛 — 未发现新的 MOVE 模式"},
           4,
           1}}},
        // ---- 4. 公共子表达式消除回放（3 步骤）----
        {"replay-cse",
         {{"🔍 CSE",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = LOAD_LOCAL slot=1   # b\n"
           "    v2 = ADD v0, v1           # a + b (表达式首次出现)\n"
           "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
           "    v4 = LOAD_LOCAL slot=0   # a\n"
           "    v5 = LOAD_LOCAL slot=1   # b\n"
           "    v6 = ADD v4, v5           # a + b (重复表达式)\n"
           "    v7 = STORE_LOCAL slot=3, v6   # y = a + b\n"
           "    end block\n"
           "}\n",
           {"建立值编号表 (value numbering table) — hash((ADD, VIRTUAL, 0, VIRTUAL, 1)) → v2",
            "扫描 v6 = ADD v4, v5 — 发现 hash((ADD, VIRTUAL, 0, VIRTUAL, 1)) 与 v2 相同",
            "暂不修改 — 等待下一 round 执行引用替换"},
           8,
           1},
          {"🔍 CSE",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = LOAD_LOCAL slot=1   # b\n"
           "    v2 = ADD v0, v1           # a + b (canonical)\n"
           "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
           "    v4 = LOAD_LOCAL slot=0   # a\n"
           "    v5 = LOAD_LOCAL slot=1   # b\n"
           "    v6 = ADD v4, v5           # 待删除 (替换 v6 → v2)\n"
           "    v7 = STORE_LOCAL slot=3, v2   # y = a + b (v6 → v2 已替换)\n"
           "    end block\n"
           "}\n",
           {"替换 STORE_LOCAL slot=3, v6 中操作数 v6 → v2 — 原因：v6 = ADD(v4, v5) 与 v2 = ADD(v0, v1) 是相同表达式",
            "保留 v6 = ADD v4, v5 原指令 — 安全策略：算术指令可能触发除零/溢出副作用，不删除指令只替换引用",
            "值编号表更新 — (ADD, VIRTUAL, 4, VIRTUAL, 5) → v2 (canonical 引用)"},
           8,
           1},
          {"🔍 CSE",
           "Round 3 (标记待删除)",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = LOAD_LOCAL slot=1   # b\n"
           "    v2 = ADD v0, v1           # canonical\n"
           "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
           "    v7 = STORE_LOCAL slot=3, v2   # y = a + b\n"
           "    end block\n"
           "}\n",
           {"标记 v4 = LOAD_LOCAL slot=0、v5 = LOAD_LOCAL slot=1、v6 = ADD v4, v5 为待删除 — 引用已被替换为 v2/v0/v1",
            "DCE 二次清理：删除 v4/v5/v6 — 原因：dest 不再被任何指令引用，纯计算无副作用",
            "指令数从 8 减少到 5 — CSE + DCE 联合优化效果"},
           5,
           3}}},
        // ---- 5. 循环展开回放（3 步骤）----
        {"replay-loop-unroll",
         {{"🔄 循环展开",
           "Round 1",
           "function main {\n"
           "  block L0:                    # start (已标记为待展开)\n"
           "    v0 = LOAD_LOCAL slot=0   # i\n"
           "    v1 = LOAD_CONST #0 (3)     # N=3 (常量边界)\n"
           "    v2 = LT v0, v1             # i < 3\n"
           "    BRANCH_FALSE v2, L2        # 跳出循环\n"
           "  block L1:                    # body\n"
           "    v3 = LOAD_LOCAL slot=1   # acc\n"
           "    v4 = LOAD_CONST #1 (1)\n"
           "    v5 = ADD v3, v4            # acc + 1\n"
           "    v6 = STORE_LOCAL slot=1, v5   # acc = acc + 1\n"
           "    v7 = LOAD_LOCAL slot=0   # i\n"
           "    v8 = LOAD_CONST #1 (1)\n"
           "    v9 = ADD v7, v8            # i + 1\n"
           "    v10 = STORE_LOCAL slot=0, v9   # i = i + 1\n"
           "    JUMP L0\n"
           "  block L2:                    # end\n"
           "    end block\n"
           "}\n",
           {"识别模式：LABEL L0; cond_load; LOAD_CONST N; LT; JUMP_IF_FALSE L2; <body>; counter_load; LOAD_CONST 1; "
            "ADD; STORE_LOCAL; JUMP L0",
            "N=3 是常量且 1 ≤ N ≤ kMaxUnrollCount(4) → 可展开",
            "body 指令数 = 8 ≤ kMaxUnrollBodySize(20) → 满足安全门；body 不含 break/continue/return/throw → "
            "安全展开；标记循环结构（L0/L1/L2 共 13 条指令）为待展开"},
           13,
           1},
          {"🔄 循环展开",
           "Round 2",
           "function main {\n"
           "  block L0:                    # 展开第 1 次\n"
           "    v3 = LOAD_LOCAL slot=1   # acc\n"
           "    v4 = LOAD_CONST #1 (1)\n"
           "    v5 = ADD v3, v4            # acc + 1\n"
           "    v6 = STORE_LOCAL slot=1, v5   # acc = acc + 1\n"
           "    v7 = LOAD_LOCAL slot=0   # i\n"
           "    v8 = LOAD_CONST #1 (1)\n"
           "    v9 = ADD v7, v8            # i + 1\n"
           "    v10 = STORE_LOCAL slot=0, v9   # i = i + 1\n"
           "  block L1:                    # 展开第 2 次\n"
           "    v11 = LOAD_LOCAL slot=1   # acc\n"
           "    v12 = LOAD_CONST #1 (1)\n"
           "    v13 = ADD v11, v12         # acc + 1\n"
           "    v14 = STORE_LOCAL slot=1, v13   # acc = acc + 1\n"
           "    v15 = LOAD_LOCAL slot=0   # i\n"
           "    v16 = LOAD_CONST #1 (1)\n"
           "    v17 = ADD v15, v16         # i + 1\n"
           "    v18 = STORE_LOCAL slot=0, v17   # i = i + 1\n"
           "  block L2:                    # 展开第 3 次\n"
           "    v19 = LOAD_LOCAL slot=1   # acc\n"
           "    v20 = LOAD_CONST #1 (1)\n"
           "    v21 = ADD v19, v20         # acc + 1\n"
           "    v22 = STORE_LOCAL slot=1, v21   # acc = acc + 1\n"
           "    v23 = LOAD_LOCAL slot=0   # i\n"
           "    v24 = LOAD_CONST #1 (1)\n"
           "    v25 = ADD v23, v24         # i + 1\n"
           "    v26 = STORE_LOCAL slot=0, v25   # i = i + 1\n"
           "    end block\n"
           "}\n",
           {"展开 N=3 次循环体 → 生成 3 份顺序 body（L0/L1/L2）",
            "vreg 重命名：第 2/3 份 body 使用新分配的 v11~v26（避免与 v3~v10 冲突）",
            "保留所有 LOAD/STORE 指令 — 循环展开后变量生命周期分析需重新进行"},
           21,
           8},
          {"🔄 循环展开",
           "Round 3 (删除原循环结构)",
           "function main {\n"
           "  block L0:                    # 展开第 1 次\n"
           "    v3 = LOAD_LOCAL slot=1   # acc\n"
           "    v4 = LOAD_CONST #1 (1)\n"
           "    v5 = ADD v3, v4            # acc + 1\n"
           "    v6 = STORE_LOCAL slot=1, v5   # acc = acc + 1\n"
           "    v7 = LOAD_LOCAL slot=0   # i\n"
           "    v8 = LOAD_CONST #1 (1)\n"
           "    v9 = ADD v7, v8            # i + 1\n"
           "    v10 = STORE_LOCAL slot=0, v9   # i = i + 1\n"
           "  block L1:                    # 展开第 2 次\n"
           "    v11 = LOAD_LOCAL slot=1   # acc\n"
           "    v12 = LOAD_CONST #1 (1)\n"
           "    v13 = ADD v11, v12         # acc + 1\n"
           "    v14 = STORE_LOCAL slot=1, v13   # acc = acc + 1\n"
           "    v15 = LOAD_LOCAL slot=0   # i\n"
           "    v16 = LOAD_CONST #1 (1)\n"
           "    v17 = ADD v15, v16         # i + 1\n"
           "    v18 = STORE_LOCAL slot=0, v17   # i = i + 1\n"
           "  block L2:                    # 展开第 3 次\n"
           "    v19 = LOAD_LOCAL slot=1   # acc\n"
           "    v20 = LOAD_CONST #1 (1)\n"
           "    v21 = ADD v19, v20         # acc + 1\n"
           "    v22 = STORE_LOCAL slot=1, v21   # acc = acc + 1\n"
           "    v23 = LOAD_LOCAL slot=0   # i\n"
           "    v24 = LOAD_CONST #1 (1)\n"
           "    v25 = ADD v23, v24         # i + 1\n"
           "    v26 = STORE_LOCAL slot=0, v25   # i = i + 1\n"
           "    end block\n"
           "}\n",
           {"删除原循环结构 — v0 = LOAD_LOCAL slot=0 (i)、v1 = LOAD_CONST #0 (3)、v2 = LT v0, v1、BRANCH_FALSE v2, "
            "L2、JUMP L0 共 5 条控制流指令",
            "删除循环结束块 L2 的占位 — 原 block L2 已被展开后的第 3 次 body 替代",
            "指令数变化：展开前 13 条 → 展开后 21 条（+8 条 = 3×7 - 13，循环开销 5 条被删除，body 复制 3 份）"},
           21,
           5}}},
    };
    return kScenarios;
}
