// ============================================================
// TestIROptReplayAudit.cpp — IR 逐步优化回放面板数据完整性审计
// ============================================================
// 覆盖 IRTransformPanel 第 4 子页（逐步优化回放）的 IROptReplayLibrary
// 静态数据类：
//   - replayScenarios().size() >= 5
//   - 每个 scenario 的 id 唯一
//   - 关键场景 ID 齐全（replay-const-fold / replay-dce / replay-copy-prop /
//     replay-cse / replay-loop-unroll）
//   - 每个 step 的 passName 非空、passRound 含 "Round"
//   - irSnapshot 含 "function" 关键字（IRToString 格式）
//   - instrCount > 0
//   - decisions 非空（至少 1 条）
//   - modifiedCount > 0（除收敛步骤，passRound 含 "收敛" 或 "报告"）
//
// 审计模式：数据完整性 + 关键字段非空 + 关键 ID 齐全 + 数值合法性
// ============================================================

#include <gtest/gtest.h>
#include "gui/IRTransformPanel.h"

#include <algorithm>
#include <set>
#include <string>

// ============================================================
// IROptReplayLibrary 审计 — 逐步优化回放场景库
// ============================================================

TEST(IROptReplayAudit, ScenariosCountAtLeast5) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    // 设计为 5 个场景：const-fold / dce / copy-prop / cse / loop-unroll
    EXPECT_GE(scenarios.size(), 5u);
}

TEST(IROptReplayAudit, ScenarioIdsUnique) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    std::set<std::string> seen;
    for (const auto& s : scenarios) {
        EXPECT_FALSE(s.first.empty()) << "场景 ID 不能为空";
        EXPECT_TRUE(seen.insert(s.first).second)
            << "场景 ID 重复: " << s.first;
    }
    EXPECT_EQ(seen.size(), scenarios.size());
}

TEST(IROptReplayAudit, CriticalScenarioIdsPresent) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) ids.insert(s.first);
    EXPECT_NE(ids.find("replay-const-fold"), ids.end());
    EXPECT_NE(ids.find("replay-dce"), ids.end());
    EXPECT_NE(ids.find("replay-copy-prop"), ids.end());
    EXPECT_NE(ids.find("replay-cse"), ids.end());
    EXPECT_NE(ids.find("replay-loop-unroll"), ids.end());
}

TEST(IROptReplayAudit, EachScenarioHas3Steps) {
    // 每个场景应至少有 3 个步骤（按设计：3 步骤/pass）
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        EXPECT_GE(s.second.size(), 3u)
            << "场景 " << s.first << " 步骤数 < 3（设计为 3 步骤）";
    }
}

TEST(IROptReplayAudit, StepPassNameNonEmpty) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            EXPECT_FALSE(step.passName.empty())
                << "场景 " << s.first << " 步骤 " << i << " passName 为空";
        }
    }
}

TEST(IROptReplayAudit, StepPassRoundContainsRoundKeyword) {
    // passRound 应含 "Round" 关键字（如 "Round 1" / "Round 2" 等）
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            EXPECT_NE(step.passRound.find("Round"), std::string::npos)
                << "场景 " << s.first << " 步骤 " << i
                << " passRound 缺少 'Round' 关键字: " << step.passRound;
        }
    }
}

TEST(IROptReplayAudit, StepIRSnapshotContainsFunctionKeyword) {
    // irSnapshot 应含 "function" 关键字（IRToString 格式）
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            EXPECT_NE(step.irSnapshot.find("function"), std::string::npos)
                << "场景 " << s.first << " 步骤 " << i
                << " irSnapshot 缺少 'function' 关键字";
        }
    }
}

TEST(IROptReplayAudit, StepInstrCountPositive) {
    // instrCount 应 > 0
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            EXPECT_GT(step.instrCount, 0)
                << "场景 " << s.first << " 步骤 " << i
                << " instrCount 应 > 0，实际为 " << step.instrCount;
        }
    }
}

TEST(IROptReplayAudit, StepDecisionsNonEmpty) {
    // decisions 应至少有 1 条
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            EXPECT_FALSE(step.decisions.empty())
                << "场景 " << s.first << " 步骤 " << i << " decisions 为空";
            for (size_t j = 0; j < step.decisions.size(); ++j) {
                EXPECT_FALSE(step.decisions[j].empty())
                    << "场景 " << s.first << " 步骤 " << i
                    << " decision " << j << " 内容为空";
            }
        }
    }
}

TEST(IROptReplayAudit, StepModifiedCountPositiveExceptConvergence) {
    // 除收敛步骤（passRound 含 "收敛" 或 "报告"）外，modifiedCount 应 > 0
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            bool isConvergence = (step.passRound.find("收敛") != std::string::npos) ||
                                 (step.passRound.find("报告") != std::string::npos);
            if (isConvergence) {
                // 收敛步骤允许 modifiedCount = 0，但不应为负
                EXPECT_GE(step.modifiedCount, 0)
                    << "场景 " << s.first << " 步骤 " << i
                    << " 收敛步骤 modifiedCount 不应为负";
            } else {
                EXPECT_GT(step.modifiedCount, 0)
                    << "场景 " << s.first << " 步骤 " << i
                    << " 非收敛步骤 modifiedCount 应 > 0，实际为 " << step.modifiedCount;
            }
        }
    }
}

TEST(IROptReplayAudit, ConstFoldScenarioFoldsAddAndMul) {
    // 验证 replay-const-fold 场景包含 ADD 折叠与 MUL 折叠
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(),
        [](const auto& s) { return s.first == "replay-const-fold"; });
    ASSERT_NE(it, scenarios.end());
    EXPECT_GE(it->second.size(), 3u);
    // 至少一个步骤的 decisions 提及 ADD 折叠
    bool foundAddFold = false;
    bool foundMulFold = false;
    for (const auto& step : it->second) {
        for (const auto& d : step.decisions) {
            if (d.find("ADD") != std::string::npos && d.find("折叠") != std::string::npos) {
                foundAddFold = true;
            }
            if (d.find("MUL") != std::string::npos && d.find("折叠") != std::string::npos) {
                foundMulFold = true;
            }
        }
    }
    EXPECT_TRUE(foundAddFold) << "replay-const-fold 应包含 ADD 折叠决策";
    EXPECT_TRUE(foundMulFold) << "replay-const-fold 应包含 MUL 折叠决策";
}

TEST(IROptReplayAudit, DCEScenarioMentionsDeadCodeElimination) {
    // 验证 replay-dce 场景包含 DCE 删除决策
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(),
        [](const auto& s) { return s.first == "replay-dce"; });
    ASSERT_NE(it, scenarios.end());
    bool foundDeletion = false;
    for (const auto& step : it->second) {
        for (const auto& d : step.decisions) {
            if (d.find("删除") != std::string::npos &&
                d.find("从未被后续指令引用") != std::string::npos) {
                foundDeletion = true;
            }
        }
    }
    EXPECT_TRUE(foundDeletion) << "replay-dce 应包含'从未被后续指令引用'删除决策";
}

TEST(IROptReplayAudit, CopyPropScenarioMentionsMovePattern) {
    // 验证 replay-copy-prop 场景包含 MOVE 模式与替换决策
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(),
        [](const auto& s) { return s.first == "replay-copy-prop"; });
    ASSERT_NE(it, scenarios.end());
    bool foundMovePattern = false;
    bool foundReplacement = false;
    for (const auto& step : it->second) {
        for (const auto& d : step.decisions) {
            if (d.find("MOVE") != std::string::npos) {
                foundMovePattern = true;
            }
            if (d.find("替换") != std::string::npos && d.find("等价复制关系") != std::string::npos) {
                foundReplacement = true;
            }
        }
    }
    EXPECT_TRUE(foundMovePattern) << "replay-copy-prop 应包含 MOVE 模式描述";
    EXPECT_TRUE(foundReplacement) << "replay-copy-prop 应包含'等价复制关系'替换决策";
}

TEST(IROptReplayAudit, CSEScenarioMentionsValueNumbering) {
    // 验证 replay-cse 场景包含值编号表与表达式替换决策
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(),
        [](const auto& s) { return s.first == "replay-cse"; });
    ASSERT_NE(it, scenarios.end());
    bool foundValueNumbering = false;
    bool foundExpressionReplace = false;
    for (const auto& step : it->second) {
        for (const auto& d : step.decisions) {
            if (d.find("值编号") != std::string::npos) {
                foundValueNumbering = true;
            }
            if (d.find("相同表达式") != std::string::npos) {
                foundExpressionReplace = true;
            }
        }
    }
    EXPECT_TRUE(foundValueNumbering) << "replay-cse 应包含值编号表描述";
    EXPECT_TRUE(foundExpressionReplace) << "replay-cse 应包含'相同表达式'替换决策";
}

TEST(IROptReplayAudit, LoopUnrollScenarioMentionsUnrollPattern) {
    // 验证 replay-loop-unroll 场景包含模式识别与展开决策
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(),
        [](const auto& s) { return s.first == "replay-loop-unroll"; });
    ASSERT_NE(it, scenarios.end());
    bool foundPattern = false;
    bool foundUnroll = false;
    for (const auto& step : it->second) {
        for (const auto& d : step.decisions) {
            if (d.find("识别模式") != std::string::npos) {
                foundPattern = true;
            }
            if (d.find("展开") != std::string::npos) {
                foundUnroll = true;
            }
        }
    }
    EXPECT_TRUE(foundPattern) << "replay-loop-unroll 应包含模式识别描述";
    EXPECT_TRUE(foundUnroll) << "replay-loop-unroll 应包含展开决策";
}

TEST(IROptReplayAudit, AllScenariosUseConsolasStyleIR) {
    // 所有 IR 快照应使用 IRToString 一致格式：包含 "block" 与 "LOAD_CONST" 或 "LOAD_LOCAL" 关键字
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        for (size_t i = 0; i < s.second.size(); ++i) {
            const auto& step = s.second[i];
            EXPECT_NE(step.irSnapshot.find("block"), std::string::npos)
                << "场景 " << s.first << " 步骤 " << i
                << " irSnapshot 缺少 'block' 关键字";
            // 至少含一个 IR 操作码（LOAD_CONST/LOAD_LOCAL/STORE_LOCAL/ADD/MUL 等）
            bool hasOp = (step.irSnapshot.find("LOAD_CONST") != std::string::npos) ||
                         (step.irSnapshot.find("LOAD_LOCAL") != std::string::npos) ||
                         (step.irSnapshot.find("STORE_LOCAL") != std::string::npos) ||
                         (step.irSnapshot.find("ADD") != std::string::npos) ||
                         (step.irSnapshot.find("MUL") != std::string::npos);
            EXPECT_TRUE(hasOp)
                << "场景 " << s.first << " 步骤 " << i
                << " irSnapshot 未识别任何 IR 操作码关键字";
        }
    }
}
