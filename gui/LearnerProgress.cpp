// ============================================================
// LearnerProgress.cpp — 学习进度持久化实现（功能 6）
// ============================================================
// 实现要点：
//   1. JSON 持久化使用 QJsonDocument / QJsonObject / QJsonArray
//   2. 文件路径：QStandardPaths::AppDataLocation + "/minilang_progress.json"
//   3. 旧版本兼容：JSON 格式不匹配时回退到空进度，不崩溃
//   4. 单例模式，提供 resetForTesting() 测试钩子
//   5. nextRecommended 算法：未完成 + 已解锁 + stage 最小，同 stage 内
//      attemptCount 最少 → estimatedMinutes 最少
// ============================================================

#include "gui/LearnerProgress.h"
// AUDIT-P1 fix: 需要 LearningPathData::stageCount() 用于 currentStage 校验上限。
#include "gui/LearningPathData.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>

// ============================================================
// 单例访问
// ============================================================
LearnerProgressStore& LearnerProgressStore::instance() {
    static LearnerProgressStore inst;
    return inst;
}

// ============================================================
// 文件路径
// ============================================================
/// 返回当前进度数据文件路径。
QString LearnerProgressStore::filePath() const {
    if (!filePathOverride_.isEmpty()) {
        return filePathOverride_;
    }
    // QStandardPaths::AppDataLocation 在 Windows 上对应
    // C:/Users/<user>/AppData/Roaming/<APPNAME>/<COMPANYNAME>
    // 当未设置 applicationName 时，回退到用户家目录
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    // 兼容旧版本：~/.minilang_progress.json
    return base + QDir::separator() + QString::fromUtf8("minilang_progress.json");
}

/// 供测试注入自定义数据文件路径，避免污染真实数据。
void LearnerProgressStore::setFilePathForTesting(const QString& path) {
    filePathOverride_ = path;
}

// ============================================================
// 加载进度
// ============================================================
/// 从文件加载进度数据；失败返回 false 并保留内存状态。
// R51-4 fix: 字段类型不匹配时拒绝加载（返回 false），保留内存中原数据。
// 原实现构建全新 loaded 对象，遇到类型不匹配的字段静默跳过（该字段在 loaded
// 中保持默认空值），最后 data_ = std::move(loaded) 整体覆盖——导致内存中
// 该字段的有效数据被默认空值覆盖丢失。修复：若某顶层字段存在但类型与预期
// 不符（如 completed 是数组而非对象），视为文件损坏，整体拒绝加载。
// 缺失字段（向后兼容旧文件）仍按默认值处理，不影响加载。
bool LearnerProgressStore::load() {
    QString path = filePath();
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        // 文件不存在或无法打开——回退到空进度，不视为错误
        return false;
    }

    QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // JSON 格式不匹配——回退到空进度，不崩溃
        return false;
    }

    LearnerProgress loaded;
    QJsonObject root = doc.object();

    // AUDIT-P2 fix: 校验 schemaVersion。缺失字段默认为 1（向后兼容旧文件），
    // 高于当前支持版本(1)则拒绝加载，避免未来 schema 变更后误读旧结构。
    // R51-4 fix: schemaVersion 若存在但非数字也视为损坏，拒绝加载。
    if (root.contains(QString::fromUtf8("schemaVersion"))) {
        QJsonValue svVal = root.value(QString::fromUtf8("schemaVersion"));
        if (!svVal.isDouble())
            return false;
    }
    int sv = root.value(QString::fromUtf8("schemaVersion")).toInt(1);
    if (sv > 1)
        return false;

    // R51-4 fix: 顶层字段类型校验辅助——字段存在但类型不符则拒绝加载
    auto requireObjectType = [&](const char* key) -> bool {
        QString qk = QString::fromUtf8(key);
        if (root.contains(qk) && !root.value(qk).isObject())
            return false;
        return true;
    };
    if (!requireObjectType("completed") || !requireObjectType("attemptCount") || !requireObjectType("lastAccessTime") ||
        !requireObjectType("levelStars") || !requireObjectType("score") || !requireObjectType("bestStars") ||
        !requireObjectType("spentMinutes") || !requireObjectType("failCount")) {
        return false;
    }
    // currentStage 若存在必须是数字
    if (root.contains(QString::fromUtf8("currentStage")) && !root.value(QString::fromUtf8("currentStage")).isDouble()) {
        return false;
    }

    // completed: { "id": true, ... }
    QJsonObject completedObj = root.value(QString::fromUtf8("completed")).toObject();
    for (auto it = completedObj.begin(); it != completedObj.end(); ++it) {
        if (it.value().isBool()) {
            loaded.completed[it.key().toStdString()] = it.value().toBool();
        }
    }

    // attemptCount: { "id": 3, ... }
    QJsonObject attemptObj = root.value(QString::fromUtf8("attemptCount")).toObject();
    for (auto it = attemptObj.begin(); it != attemptObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: 防御性校验——手动篡改的 JSON 可能含负数，
            // 负数 attemptCount 会破坏 nextRecommended 排序语义。
            int v = it.value().toInt();
            if (v >= 0) {
                loaded.attemptCount[it.key().toStdString()] = v;
            }
        }
    }

    // lastAccessTime: { "id": 1700000000, ... }
    QJsonObject accessObj = root.value(QString::fromUtf8("lastAccessTime")).toObject();
    for (auto it = accessObj.begin(); it != accessObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: 负数时间戳兜底为 0，与其他数值字段的范围校验保持一致，
            // 防止手动篡改的 JSON 破坏 nextRecommended 的时间排序语义。
            int64_t ts = static_cast<int64_t>(it.value().toDouble());
            if (ts < 0)
                ts = 0;
            loaded.lastAccessTime[it.key().toStdString()] = ts;
        }
    }

    // currentStage: int
    // AUDIT-P1 fix: 校验上限改为 stageCount()（5），允许"全部 100% 完成"的合法状态。
    // 原校验 s <= 4 拒绝 recomputeStage 在全通关时设置的 currentStage=5，
    // 导致通关用户跨会话恢复时进度被重置为 0，"🎉 通关"提示丢失。
    if (root.contains(QString::fromUtf8("currentStage"))) {
        QJsonValue v = root.value(QString::fromUtf8("currentStage"));
        if (v.isDouble()) {
            int s = v.toInt();
            if (s >= 0 && s <= LearningPathData::stageCount()) {
                loaded.currentStage = s;
            }
        }
    }

    // P0-2 fix (F7): levelStars: { "token-puzzle-1": 3, ... }
    QJsonObject starsObj = root.value(QString::fromUtf8("levelStars")).toObject();
    for (auto it = starsObj.begin(); it != starsObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: levelStars 合法范围 [-1, 3]（-1=未完成,0=跳过,1-3=星级）
            int v = it.value().toInt();
            if (v >= -1 && v <= 3) {
                loaded.levelStars[it.key().toStdString()] = v;
            }
        }
    }

    // P2-3 fix (F9): 学情画像多维进度数据（向后兼容——缺失字段按空 map 处理）
    // score: { "lab-01": 85, ... }
    QJsonObject scoreObj = root.value(QString::fromUtf8("score")).toObject();
    for (auto it = scoreObj.begin(); it != scoreObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: score 合法范围 [0, 100]，与 recordScore 截断逻辑一致
            int v = it.value().toInt();
            if (v >= 0 && v <= 100) {
                loaded.score[it.key().toStdString()] = v;
            }
        }
    }
    // bestStars: { "lab-01": 3, ... }
    QJsonObject bestStarsObj = root.value(QString::fromUtf8("bestStars")).toObject();
    for (auto it = bestStarsObj.begin(); it != bestStarsObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: bestStars 合法范围 [0, 3]
            int v = it.value().toInt();
            if (v >= 0 && v <= 3) {
                loaded.bestStars[it.key().toStdString()] = v;
            }
        }
    }
    // spentMinutes: { "lab-01": 12, ... }
    QJsonObject spentObj = root.value(QString::fromUtf8("spentMinutes")).toObject();
    for (auto it = spentObj.begin(); it != spentObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: spentMinutes 不能为负，与 addSpentMinutes 拒绝负数一致
            int v = it.value().toInt();
            if (v >= 0) {
                loaded.spentMinutes[it.key().toStdString()] = v;
            }
        }
    }
    // failCount: { "lab-02": 2, ... }
    QJsonObject failObj = root.value(QString::fromUtf8("failCount")).toObject();
    for (auto it = failObj.begin(); it != failObj.end(); ++it) {
        if (it.value().isDouble()) {
            // AUDIT-P2 fix: failCount 不能为负，负数会破坏 getWeakPoints 排序
            int v = it.value().toInt();
            if (v >= 0) {
                loaded.failCount[it.key().toStdString()] = v;
            }
        }
    }

    data_ = std::move(loaded);
    return true;
}

// ============================================================
// 保存进度
// ============================================================
/// 将内存中的进度数据写回文件。
// AUDIT-P1 fix: 原子写入——写临时文件 → flush → close → remove 旧 → rename，
// 避免崩溃/断电/磁盘满导致目标文件被截断为部分内容或空文件，全部进度丢失。
// AUDIT-P2 fix: 写入 schemaVersion 字段，支持未来 schema 迁移。
// R51-3 fix: 改用 QSaveFile 替代手写 remove→rename 序列。原实现先 remove 旧文件
// 再 rename，在两步之间崩溃会导致目标文件丢失（旧文件已删、新文件未就位）。
// QSaveFile::commit() 在 Windows 上使用 MOVEFILE_REPLACE_EXISTING 原子替换，
// 在 Linux 上用 rename(2) 原子覆盖，消除数据丢失窗口。
bool LearnerProgressStore::save() const {
    QString path = filePath();
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());

    QJsonObject root;

    // AUDIT-P2 fix: schema 版本号，支持未来字段重命名/类型变更/语义变更的迁移
    root.insert(QString::fromUtf8("schemaVersion"), 1);

    QJsonObject completedObj;
    for (const auto& [id, val] : data_.completed) {
        completedObj.insert(QString::fromStdString(id), val);
    }
    root.insert(QString::fromUtf8("completed"), completedObj);

    QJsonObject attemptObj;
    for (const auto& [id, val] : data_.attemptCount) {
        attemptObj.insert(QString::fromStdString(id), val);
    }
    root.insert(QString::fromUtf8("attemptCount"), attemptObj);

    QJsonObject accessObj;
    for (const auto& [id, val] : data_.lastAccessTime) {
        // int64_t 转为 double 存储（JSON 数字精度）
        accessObj.insert(QString::fromStdString(id), static_cast<double>(val));
    }
    root.insert(QString::fromUtf8("lastAccessTime"), accessObj);

    root.insert(QString::fromUtf8("currentStage"), data_.currentStage);

    // P0-2 fix (F7): 持久化细粒度关卡星级
    QJsonObject starsObj;
    for (const auto& [id, stars] : data_.levelStars) {
        starsObj.insert(QString::fromStdString(id), stars);
    }
    root.insert(QString::fromUtf8("levelStars"), starsObj);

    // P2-3 fix (F9): 持久化学情画像多维进度数据
    QJsonObject scoreObj;
    for (const auto& [id, sc] : data_.score) {
        scoreObj.insert(QString::fromStdString(id), sc);
    }
    root.insert(QString::fromUtf8("score"), scoreObj);

    QJsonObject bestStarsObj;
    for (const auto& [id, stars] : data_.bestStars) {
        bestStarsObj.insert(QString::fromStdString(id), stars);
    }
    root.insert(QString::fromUtf8("bestStars"), bestStarsObj);

    QJsonObject spentObj;
    for (const auto& [id, mins] : data_.spentMinutes) {
        spentObj.insert(QString::fromStdString(id), mins);
    }
    root.insert(QString::fromUtf8("spentMinutes"), spentObj);

    QJsonObject failObj;
    for (const auto& [id, fails] : data_.failCount) {
        failObj.insert(QString::fromStdString(id), fails);
    }
    root.insert(QString::fromUtf8("failCount"), failObj);

    QJsonDocument doc(root);

    // R51-3 fix: 使用 QSaveFile 实现真正的原子写入。
    // QSaveFile 在 commit() 前写入临时文件，commit() 时原子替换目标文件。
    // 若 commit 前崩溃/断电，目标文件保持原样不受影响。
    // directWriteFallback=false：禁止 QSaveFile 在无法原子替换时直接写目标文件
    // （直接写会在崩溃时留下截断文件）。
    QSaveFile sf(path);
    sf.setDirectWriteFallback(false);
    if (!sf.open(QIODevice::WriteOnly)) {
        return false;
    }
    QByteArray payload = doc.toJson(QJsonDocument::Indented);
    qint64 written = sf.write(payload);
    if (written != static_cast<qint64>(payload.size())) {
        sf.cancelWriting();
        return false;
    }
    if (!sf.commit()) {
        return false;
    }
    return true;
}

// ============================================================
// 修改接口
// ============================================================
/// 标记某活动已完成并记录时间戳。
void LearnerProgressStore::markCompleted(const std::string& activityId) {
    if (activityId.empty())
        return;
    data_.completed[activityId] = true;
    auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    data_.lastAccessTime[activityId] = now;
    // P1-2 fix (F8): 完成状态变化后立即重算阶段，避免 currentStage 永远为 0。
    // 此处调用 recomputeStage 是幂等的——多次调用只覆盖 currentStage，无副作用。
    recomputeStage(LearningPathData::activities());
}

// ============================================================
// P1-2 fix (F8): 重算 currentStage
// 规则：
//   - 遍历 stage 0..N，找出"完成比例 >= 50%"的最高阶段 highestCompletedStage
//   - 若所有阶段都 >= 50%，currentStage = stageCount（已通关所有阶段）
//   - 否则 currentStage = highestCompletedStage + 1（下一未完成阶段）
//   - 若所有阶段都 < 50%，currentStage = 0（仍是新手阶段）
// 注：阈值 50% 的取值平衡"完成感"与"鼓励性"——过半即解锁下一阶段提示。
// ============================================================
/// 依据全部活动重算各阶段完成状态。
void LearnerProgressStore::recomputeStage(const std::vector<LearningActivity>& all) {
    if (all.empty()) {
        data_.currentStage = 0;
        return;
    }
    int highestQualified = -1; // 完成比例 >= 50% 的最高阶段
    for (int stage = 0; stage < LearningPathData::stageCount(); ++stage) {
        int sp = stageProgress(stage, all);
        if (sp >= 50) {
            highestQualified = stage;
        }
    }
    if (highestQualified < 0) {
        // 没有任何阶段过半，仍在阶段 0
        data_.currentStage = 0;
    } else if (highestQualified == LearningPathData::stageCount() - 1) {
        // 最后一阶段也过半——若已 100% 则标记为 stageCount（通关），否则停留在最后阶段
        int lastStageProgress = stageProgress(highestQualified, all);
        data_.currentStage = (lastStageProgress >= 100) ? LearningPathData::stageCount() : highestQualified;
    } else {
        // 下一未完成阶段
        data_.currentStage = highestQualified + 1;
    }
}

/// 记录一次尝试（即使未完成也计数）。
void LearnerProgressStore::recordAttempt(const std::string& activityId) {
    if (activityId.empty())
        return;
    data_.attemptCount[activityId] += 1;
    auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    data_.lastAccessTime[activityId] = now;
}

/// 清空全部进度数据。
void LearnerProgressStore::reset() {
    data_.completed.clear();
    data_.attemptCount.clear();
    data_.lastAccessTime.clear();
    data_.currentStage = 0;
    // P0-2 fix (F7): 同步清空细粒度关卡星级
    data_.levelStars.clear();
    // P2-3 fix (F9): 同步清空学情画像多维数据
    data_.score.clear();
    data_.bestStars.clear();
    data_.spentMinutes.clear();
    data_.failCount.clear();
}

// ============================================================
// P0-2 fix (F7): 关卡星级接口实现
// ============================================================
/// 为某关卡记录星级（0-3）。
void LearnerProgressStore::markLevelStars(const std::string& levelId, int stars) {
    if (levelId.empty())
        return;
    // 仅当新星级 >= 已记录星级时才覆盖（保留历史最佳成绩）
    auto it = data_.levelStars.find(levelId);
    if (it == data_.levelStars.end() || stars > it->second) {
        data_.levelStars[levelId] = stars;
    }
    // stars >= 0 表示关卡至少被完成或跳过，同步记录访问时间
    if (stars >= 0) {
        auto now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        data_.lastAccessTime[levelId] = now;
    }
}

/// 读取某关卡的星级。
int LearnerProgressStore::getLevelStars(const std::string& levelId) const {
    auto it = data_.levelStars.find(levelId);
    if (it == data_.levelStars.end())
        return -1; // 未记录 = 未完成
    return it->second;
}

/// 判断给定关卡是否全部完成。
bool LearnerProgressStore::areAllLevelsCompleted(const std::vector<std::string>& levelIds) const {
    if (levelIds.empty())
        return false;
    for (const auto& id : levelIds) {
        auto it = data_.levelStars.find(id);
        if (it == data_.levelStars.end() || it->second < 0) {
            return false;
        }
    }
    return true;
}

// ============================================================
// P2-3 fix (F9): 学情画像接口实现
// ============================================================
/// 记录某活动得分与星级。
void LearnerProgressStore::recordScore(const std::string& activityId, int score, int stars) {
    if (activityId.empty())
        return;
    // score 截断到 [0, 100]
    if (score < 0)
        score = 0;
    if (score > 100)
        score = 100;
    // 保留历史最佳得分（仅当新分 >= 旧分时覆盖）
    auto it = data_.score.find(activityId);
    if (it == data_.score.end() || score > it->second) {
        data_.score[activityId] = score;
    }
    // 保留历史最佳星级（仅当新星级 >= 旧星级时覆盖）
    if (stars > 0) {
        auto sit = data_.bestStars.find(activityId);
        if (sit == data_.bestStars.end() || stars > sit->second) {
            data_.bestStars[activityId] = stars;
        }
    }
    // 同步更新访问时间
    auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    data_.lastAccessTime[activityId] = now;
}

/// 读取某活动得分。
int LearnerProgressStore::getScore(const std::string& activityId) const {
    auto it = data_.score.find(activityId);
    return it != data_.score.end() ? it->second : 0;
}

/// 读取某活动历史最佳星级。
int LearnerProgressStore::getBestStars(const std::string& activityId) const {
    auto it = data_.bestStars.find(activityId);
    return it != data_.bestStars.end() ? it->second : 0;
}

/// 累加某活动投入分钟数。
void LearnerProgressStore::addSpentMinutes(const std::string& activityId, int minutes) {
    if (activityId.empty())
        return;
    if (minutes <= 0)
        return; // 负数或零视为无操作
    data_.spentMinutes[activityId] += minutes;
    // 不更新 lastAccessTime——时间累加是异步操作，不应刷新访问时间戳
}

/// 读取某活动累计分钟数。
int LearnerProgressStore::getSpentMinutes(const std::string& activityId) const {
    auto it = data_.spentMinutes.find(activityId);
    return it != data_.spentMinutes.end() ? it->second : 0;
}

/// 记录一次失败（失败计数 +1）。
void LearnerProgressStore::recordFailure(const std::string& activityId) {
    if (activityId.empty())
        return;
    data_.failCount[activityId] += 1;
    auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    data_.lastAccessTime[activityId] = now;
}

/// 读取某活动失败次数。
int LearnerProgressStore::getFailCount(const std::string& activityId) const {
    auto it = data_.failCount.find(activityId);
    return it != data_.failCount.end() ? it->second : 0;
}

// ============================================================
// P2-3 fix (F9): 薄弱点分析
// 算法：
//   1. 遍历所有活动，筛选"未完成 + (attemptCount >= minAttempts 或 failCount >= 1)"
//   2. 按 failCount 降序 → attemptCount 降序 → id 字典序排序
//   3. 截取前 maxCount 个返回（maxCount=0 表示不限制）
// ============================================================
/// 基于失败/低星识别薄弱点列表。
std::vector<WeakPoint> LearnerProgressStore::getWeakPoints(const std::vector<LearningActivity>& all, int minAttempts,
                                                           std::size_t maxCount) const {

    std::vector<WeakPoint> candidates;
    for (const auto& a : all) {
        // 已完成则跳过——薄弱点仅针对未通过活动
        auto cit = data_.completed.find(a.id);
        if (cit != data_.completed.end() && cit->second)
            continue;

        int attempts = 0;
        auto ait = data_.attemptCount.find(a.id);
        if (ait != data_.attemptCount.end())
            attempts = ait->second;

        int fails = 0;
        auto fit = data_.failCount.find(a.id);
        if (fit != data_.failCount.end())
            fails = fit->second;

        int sc = 0;
        auto sit = data_.score.find(a.id);
        if (sit != data_.score.end())
            sc = sit->second;

        // 筛选条件：尝试次数 >= 阈值 或 至少失败过一次
        if (attempts >= minAttempts || fails >= 1) {
            candidates.push_back(WeakPoint{a.id, attempts, fails, sc});
        }
    }

    // 排序：失败次数降序 → 尝试次数降序 → id 字典序
    std::sort(candidates.begin(), candidates.end(), [](const WeakPoint& a, const WeakPoint& b) {
        if (a.fails != b.fails)
            return a.fails > b.fails;
        if (a.attempts != b.attempts)
            return a.attempts > b.attempts;
        return a.activityId < b.activityId;
    });

    // 截取前 maxCount 个（maxCount=0 表示不限制）
    if (maxCount > 0 && candidates.size() > maxCount) {
        candidates.resize(maxCount);
    }
    return candidates;
}

// ============================================================
// P2-3 fix (F9): 预计剩余时间
// 算法：sum(未完成且已解锁活动的 estimatedMinutes)
// 注：未解锁活动不计入——因为它们当前无法开始，不应计入"剩余"。
// ============================================================
/// 估算完成剩余活动所需分钟数。
int LearnerProgressStore::estimatedRemainingMinutes(const std::vector<LearningActivity>& all) const {
    int total = 0;
    for (const auto& a : all) {
        // 已完成则跳过
        auto cit = data_.completed.find(a.id);
        if (cit != data_.completed.end() && cit->second)
            continue;

        // 未解锁则跳过（不计入剩余时间）
        if (!isUnlocked(a.id, all))
            continue;

        total += a.estimatedMinutes;
    }
    return total;
}

/// 返回总投入分钟数。
int LearnerProgressStore::totalSpentMinutes() const {
    int total = 0;
    for (const auto& [id, mins] : data_.spentMinutes) {
        total += mins;
    }
    return total;
}

// ============================================================
// 查询接口
// ============================================================
/// 判断某活动是否已解锁（前置依赖满足）。
bool LearnerProgressStore::isUnlocked(const std::string& activityId, const std::vector<LearningActivity>& all) const {
    // 找到该活动
    const LearningActivity* act = nullptr;
    for (const auto& a : all) {
        if (a.id == activityId) {
            act = &a;
            break;
        }
    }
    if (!act)
        return false;

    // 检查所有前置是否完成
    for (const auto& preId : act->prerequisites) {
        auto it = data_.completed.find(preId);
        if (it == data_.completed.end() || !it->second) {
            return false;
        }
    }
    return true;
}

/// 返回某阶段的完成进度百分比。
int LearnerProgressStore::stageProgress(int stage, const std::vector<LearningActivity>& all) const {
    int total = 0;
    int done = 0;
    for (const auto& a : all) {
        if (a.stage != stage)
            continue;
        ++total;
        auto it = data_.completed.find(a.id);
        if (it != data_.completed.end() && it->second)
            ++done;
    }
    if (total == 0)
        return 0;
    return static_cast<int>((static_cast<int64_t>(done) * 100) / total);
}

/// 返回总体完成进度百分比。
int LearnerProgressStore::overallProgress(const std::vector<LearningActivity>& all) const {
    if (all.empty())
        return 0;
    int total = static_cast<int>(all.size());
    int done = 0;
    for (const auto& a : all) {
        auto it = data_.completed.find(a.id);
        if (it != data_.completed.end() && it->second)
            ++done;
    }
    return static_cast<int>((static_cast<int64_t>(done) * 100) / total);
}

/// 返回下一个推荐学习活动的 id/描述。
std::string LearnerProgressStore::nextRecommended(const std::vector<LearningActivity>& all) const {

    // 1. 筛选候选：未完成 + 已解锁
    std::vector<const LearningActivity*> candidates;
    for (const auto& a : all) {
        // 已完成则跳过
        auto cit = data_.completed.find(a.id);
        if (cit != data_.completed.end() && cit->second)
            continue;

        // 检查所有前置完成
        bool unlocked = true;
        for (const auto& preId : a.prerequisites) {
            auto pit = data_.completed.find(preId);
            if (pit == data_.completed.end() || !pit->second) {
                unlocked = false;
                break;
            }
        }
        if (unlocked)
            candidates.push_back(&a);
    }

    if (candidates.empty())
        return std::string();

    // 2. 排序：stage 升序 → attemptCount 升序 → estimatedMinutes 升序 → id 字典序
    auto attemptOf = [this](const std::string& id) -> int {
        auto it = data_.attemptCount.find(id);
        return it != data_.attemptCount.end() ? it->second : 0;
    };

    std::sort(candidates.begin(), candidates.end(), [&attemptOf](const LearningActivity* a, const LearningActivity* b) {
        if (a->stage != b->stage)
            return a->stage < b->stage;
        int aa = attemptOf(a->id);
        int ab = attemptOf(b->id);
        if (aa != ab)
            return aa < ab;
        if (a->estimatedMinutes != b->estimatedMinutes)
            return a->estimatedMinutes < b->estimatedMinutes;
        return a->id < b->id;
    });

    return candidates.front()->id;
}
