#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// GlobalSlotAllocator — 全局变量槽位分配器（B4 优化）
// ------------------------------------------------------------
// 提取自 Compiler 和 AstIRBuilder 的共同逻辑：
//   维护 name ↔ slot 双向映射 + 槽位回收
//   - globalSlots_: name → slot index（O(1) 查找）
//   - slotNames_:   slot → name（平行数组，最终传递给 VM）
//   - freeSlots_:   回收的槽位索引（支持复用）
//
// Compiler 和 AstIRBuilder 各持一个实例，消除两套重复实现。
// ============================================================

class GlobalSlotAllocator {
public:
    /// 分配全局槽位（已有则返回现有，否则从 freeSlots_ 或新分配）
    int allocate(const std::string& name) {
        auto it = slots_.find(name);
        if (it != slots_.end())
            return it->second;
        int slot;
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
            if (slot < static_cast<int>(names_.size())) {
                names_[slot] = name;
            }
        } else {
            slot = static_cast<int>(names_.size());
            names_.push_back(name);
        }
        slots_[name] = slot;
        return slot;
    }

    /// 释放全局槽位（从 slots_ 移除，推入 freeSlots_，清空 names_ 对应位置）
    void release(const std::string& name) {
        auto it = slots_.find(name);
        if (it == slots_.end())
            return;
        int slot = it->second;
        slots_.erase(it);
        if (slot < static_cast<int>(names_.size())) {
            names_[slot] = "";
        }
        freeSlots_.push_back(slot);
    }

    /// 查找全局槽位（未找到返回 -1）
    int lookup(const std::string& name) const {
        auto it = slots_.find(name);
        return (it != slots_.end()) ? it->second : -1;
    }

    /// B4: 临时移除槽位映射（保留 names_ 中的名称，仅从 slots_ map 删除）
    /// 用于 catch 变量遮蔽和块作用域遮蔽场景。返回原槽位号（-1 表示未找到）。
    /// 注意：与 release() 不同，此方法不回收槽位到 freeSlots_，也不清空 names_[slot]，
    /// 因为遮蔽是临时的，稍后会用 restoreMapping() 恢复。
    int removeMapping(const std::string& name) {
        auto it = slots_.find(name);
        if (it == slots_.end())
            return -1;
        int slot = it->second;
        slots_.erase(it);
        return slot;
    }

    /// B4: 恢复之前 removeMapping 移除的槽位映射
    void restoreMapping(const std::string& name, int slot) { slots_[name] = slot; }

    /// 取槽位名表（slot → name，最终传递给 CompileResult）
    const std::vector<std::string>& names() const { return names_; }

    /// 取槽位名表的可变引用（用于 std::move 给 CompileResult）
    std::vector<std::string>& mutableNames() { return names_; }

    /// 槽位数量
    int count() const { return static_cast<int>(names_.size()); }

    /// 清空所有状态
    void clear() {
        slots_.clear();
        names_.clear();
        freeSlots_.clear();
    }

private:
    std::unordered_map<std::string, int> slots_; // name → slot index
    std::vector<std::string> names_;             // slot → name (parallel array)
    std::vector<int> freeSlots_;                 // recycled slot indices
};
