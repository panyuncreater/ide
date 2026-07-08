// ============================================================
// TokenPuzzleData.h — 交互式 Token 拼图游戏关卡数据定义（功能 3）
// ------------------------------------------------------------
// 通过游戏化方式让学习者理解"词法分析就是切分字符流"。
// 玩家将打乱的 token 按正确顺序排列，还原目标语句。
//
// 本文件仅包含数据结构与静态数据接口声明，不依赖 IdeController.h
// / 引擎层 / Qt Widgets——与 BugHuntLibrary / LearningPathData 的
// 设计模式一致，便于测试目标独立链接。
//
// 关卡设计（由浅入深，5 关）：
//   1. var x = 42;                       — 基本 5 个 token          ⭐
//   2. print("hello");                   — 字符串是一个 token       ⭐
//   3. x > 0 and y < 10;                 — 运算符优先级不影响切分   ⭐⭐
//   4. // 这是注释\nprint(1);            — 注释被分离出主流         ⭐⭐
//   5. a[0] = b["key"] + 1;              — 复杂表达式               ⭐⭐⭐
//
// shuffledTokens 为 tokens 的固定乱序（硬编码，非运行时随机），
// 保证可重现并满足"确实打乱"的测试断言。
// ============================================================

#pragma once

#include <string>
#include <vector>

// ============================================================
// TokenPuzzleLevel — 单个关卡数据
// ============================================================
struct TokenPuzzleLevel {
    int level;                               // 关卡编号 1-5
    std::string targetCode;                  // 目标语句（如 "var x = 42;"）
    std::vector<std::string> tokens;         // 正确顺序的 token 字面量
    std::vector<std::string> shuffledTokens; // 打乱后的 token（供玩家点击）
    std::string teachingPoint;               // 教学点说明
    std::string hint;                        // 提示文本
    int difficulty;                          // 难度 1-3（⭐~⭐⭐⭐）
};

// ============================================================
// TokenPuzzleLibrary — 关卡静态数据
// ------------------------------------------------------------
// 所有关卡数据嵌入 .cpp 文件，与 BugHuntLibrary.cpp 模式一致。
// 通过 levels() 接口提供只读访问，首次调用时初始化、后续
// 调用零开销。
// ============================================================
class TokenPuzzleLibrary {
public:
    /// 返回所有关卡（按 level 1-5 排序）
    static const std::vector<TokenPuzzleLevel>& levels();

    /// 关卡总数
    static int levelCount() { return 5; }
};
