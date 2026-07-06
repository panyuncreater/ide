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

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDateTime>

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

void LearnerProgressStore::setFilePathForTesting(const QString& path) {
    filePathOverride_ = path;
}

// ============================================================
// 加载进度
// ============================================================
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
            loaded.attemptCount[it.key().toStdString()] = it.value().toInt();
        }
    }

    // lastAccessTime: { "id": 1700000000, ... }
    QJsonObject accessObj = root.value(QString::fromUtf8("lastAccessTime")).toObject();
    for (auto it = accessObj.begin(); it != accessObj.end(); ++it) {
        if (it.value().isDouble()) {
            loaded.lastAccessTime[it.key().toStdString()] =
                static_cast<int64_t>(it.value().toDouble());
        }
    }

    // currentStage: int
    if (root.contains(QString::fromUtf8("currentStage"))) {
        QJsonValue v = root.value(QString::fromUtf8("currentStage"));
        if (v.isDouble()) {
            int s = v.toInt();
            if (s >= 0 && s <= 4) {
                loaded.currentStage = s;
            }
        }
    }

    data_ = std::move(loaded);
    return true;
}

// ============================================================
// 保存进度
// ============================================================
bool LearnerProgressStore::save() const {
    QString path = filePath();
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());

    QJsonObject root;

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
        accessObj.insert(QString::fromStdString(id),
                         static_cast<double>(val));
    }
    root.insert(QString::fromUtf8("lastAccessTime"), accessObj);

    root.insert(QString::fromUtf8("currentStage"), data_.currentStage);

    QJsonDocument doc(root);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

// ============================================================
// 修改接口
// ============================================================
void LearnerProgressStore::markCompleted(const std::string& activityId) {
    if (activityId.empty()) return;
    data_.completed[activityId] = true;
    auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    data_.lastAccessTime[activityId] = now;
}

void LearnerProgressStore::recordAttempt(const std::string& activityId) {
    if (activityId.empty()) return;
    data_.attemptCount[activityId] += 1;
    auto now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    data_.lastAccessTime[activityId] = now;
}

void LearnerProgressStore::reset() {
    data_.completed.clear();
    data_.attemptCount.clear();
    data_.lastAccessTime.clear();
    data_.currentStage = 0;
}

// ============================================================
// 查询接口
// ============================================================
bool LearnerProgressStore::isUnlocked(const std::string& activityId,
                                       const std::vector<LearningActivity>& all) const {
    // 找到该活动
    const LearningActivity* act = nullptr;
    for (const auto& a : all) {
        if (a.id == activityId) { act = &a; break; }
    }
    if (!act) return false;

    // 检查所有前置是否完成
    for (const auto& preId : act->prerequisites) {
        auto it = data_.completed.find(preId);
        if (it == data_.completed.end() || !it->second) {
            return false;
        }
    }
    return true;
}

int LearnerProgressStore::stageProgress(int stage,
                                         const std::vector<LearningActivity>& all) const {
    int total = 0;
    int done = 0;
    for (const auto& a : all) {
        if (a.stage != stage) continue;
        ++total;
        auto it = data_.completed.find(a.id);
        if (it != data_.completed.end() && it->second) ++done;
    }
    if (total == 0) return 0;
    return static_cast<int>((static_cast<int64_t>(done) * 100) / total);
}

int LearnerProgressStore::overallProgress(const std::vector<LearningActivity>& all) const {
    if (all.empty()) return 0;
    int total = static_cast<int>(all.size());
    int done = 0;
    for (const auto& a : all) {
        auto it = data_.completed.find(a.id);
        if (it != data_.completed.end() && it->second) ++done;
    }
    return static_cast<int>((static_cast<int64_t>(done) * 100) / total);
}

std::string LearnerProgressStore::nextRecommended(
    const std::vector<LearningActivity>& all) const {

    // 1. 筛选候选：未完成 + 已解锁
    std::vector<const LearningActivity*> candidates;
    for (const auto& a : all) {
        // 已完成则跳过
        auto cit = data_.completed.find(a.id);
        if (cit != data_.completed.end() && cit->second) continue;

        // 检查所有前置完成
        bool unlocked = true;
        for (const auto& preId : a.prerequisites) {
            auto pit = data_.completed.find(preId);
            if (pit == data_.completed.end() || !pit->second) {
                unlocked = false;
                break;
            }
        }
        if (unlocked) candidates.push_back(&a);
    }

    if (candidates.empty()) return std::string();

    // 2. 排序：stage 升序 → attemptCount 升序 → estimatedMinutes 升序 → id 字典序
    auto attemptOf = [this](const std::string& id) -> int {
        auto it = data_.attemptCount.find(id);
        return it != data_.attemptCount.end() ? it->second : 0;
    };

    std::sort(candidates.begin(), candidates.end(),
        [&attemptOf](const LearningActivity* a, const LearningActivity* b) {
            if (a->stage != b->stage) return a->stage < b->stage;
            int aa = attemptOf(a->id);
            int ab = attemptOf(b->id);
            if (aa != ab) return aa < ab;
            if (a->estimatedMinutes != b->estimatedMinutes)
                return a->estimatedMinutes < b->estimatedMinutes;
            return a->id < b->id;
        });

    return candidates.front()->id;
}
