// ============================================================
// ProgressSaveFeedback.h — 学习进度保存失败的统一 UI 反馈 helper
// ------------------------------------------------------------
// P2-UX fix 推广：LearnerProgressStore::save() 失败（磁盘满/权限不足/
// 路径不可写等）时仅返回 false，原各面板调用点未检查返回值导致进度
// 静默丢失。LabManualPanel 首先引入"失败时弹 InfoBar 警告"模式
// （saveProgressWithFeedback 成员函数），本 header 将该模式提取为
// 共享自由函数，供全部教学面板调用点复用，消除 8 处忽略返回值的
// 重复模式。
//
// 依赖说明：本 header 引入 QFluent InfoBar（widget 依赖），仅供
// minilang_ide 目标内的面板 include；minilang_tests 目标只编译
// LearnerProgress.cpp（无 UI 依赖），不 include 本 header。
// ============================================================
#pragma once

#include "QFluent/InfoBar.h"
#include "gui/I18n.h"
#include "gui/LearnerProgress.h"

#include <QWidget>

/// 持久化学习进度并在失败时弹出 InfoBar 警告通知用户。
/// @param parent toast 挂靠的父 widget（面板 this）
inline void saveLearnerProgressWithFeedback(QWidget* parent) {
    if (LearnerProgressStore::instance().save()) {
        return;
    }
    // 进度保存失败时弹出 toast 通知，避免静默丢失
    InfoBar::warning(mlTr("进度保存失败"), mlTr("无法保存学习进度，请检查文件权限或磁盘空间"), Qt::Horizontal, true,
                     3000, InfoBar::Position::TOP_RIGHT, parent);
}
