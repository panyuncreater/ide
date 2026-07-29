#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

// ============================================================
// SmallMap: 内联平坦存储优化的小型映射容器
// ============================================================
// 当条目数 ≤ N 时使用内联数组（线性扫描），超过 N 时回退到 std::unordered_map。
// 针对 Environment 变量存储优化：大多数作用域仅有 1-8 个变量，
// 内联模式避免了 unordered_map 的桶数组堆分配（~64 字节最小开销）。
// 线性扫描对于 ≤8 条目比哈希更快（缓存友好，无哈希计算开销）。
//
// AUDIT-R3 P2 契约警示（与 std::unordered_map 的关键差异）：
//   1. 引用/迭代器失效：第 N+1 次插入触发内联→堆晋升（promoteToHeap），
//      此前通过 operator[]/try_emplace/find 获得的引用与迭代器全部失效
//      （仍指向已被重置为默认值的内联槽位：读到默认值、写入静默丢失）。
//      unordered_map 保证 rehash 后引用仍有效，本容器不保证。
//      调用方若需长期持有 V*（如 Environment::boundInstance_），必须保证
//      持有期间不再新增条目，或在插入后重新定位（参考 H2 fix）。
//   2. erase 为换尾删除（O(1) 不保序）：遍历中删除会漏访/重访元素，
//      不要在迭代中 erase。

template <typename K, typename V, size_t N = 8> class SmallMap {
public:
    using value_type = std::pair<K, V>;
    using size_type = size_t;
    using MapType = std::unordered_map<K, V>;

    // ===== Iterator =====
    class iterator {
        friend class SmallMap;
        value_type* ptr_ = nullptr;
        typename MapType::iterator mapIt_{};
        bool heapMode_ = false;

        explicit iterator(value_type* p) : ptr_(p), heapMode_(false) {}
        explicit iterator(typename MapType::iterator it) : mapIt_(it), heapMode_(true) {}

    public:
        iterator() = default;

        value_type& operator*() const {
            // Bug #45 note: reinterpret_cast between std::pair<const K,V> and
            // std::pair<K,V> is technically a strict-aliasing violation, but both
            // are standard-layout types with identical size/alignment/representation.
            // This is safe on all target compilers (MSVC, GCC, Clang) which guarantee
            // layout-compatible standard-layout types may be interconverted.
            // A fully conforming fix would require changing value_type to
            // std::pair<const K,V> (breaking the inline array API) or adding a
            // projection layer; the cost/benefit does not justify that change here.
            if (heapMode_)
                return reinterpret_cast<value_type&>(*mapIt_);
            return *ptr_;
        }
        value_type* operator->() const {
            if (heapMode_)
                return reinterpret_cast<value_type*>(&(*mapIt_));
            return ptr_;
        }

        iterator& operator++() {
            if (heapMode_)
                ++mapIt_;
            else
                ++ptr_;
            return *this;
        }
        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const iterator& o) const {
            if (heapMode_ != o.heapMode_)
                return false;
            return heapMode_ ? (mapIt_ == o.mapIt_) : (ptr_ == o.ptr_);
        }
        bool operator!=(const iterator& o) const { return !(*this == o); }
    };

    class const_iterator {
        friend class SmallMap;
        const value_type* ptr_ = nullptr;
        typename MapType::const_iterator mapIt_{};
        bool heapMode_ = false;

        explicit const_iterator(const value_type* p) : ptr_(p), heapMode_(false) {}
        explicit const_iterator(typename MapType::const_iterator it) : mapIt_(it), heapMode_(true) {}

    public:
        const_iterator() = default;

        const value_type& operator*() const {
            if (heapMode_)
                return reinterpret_cast<const value_type&>(*mapIt_);
            return *ptr_;
        }
        const value_type* operator->() const {
            if (heapMode_)
                return reinterpret_cast<const value_type*>(&(*mapIt_));
            return ptr_;
        }

        const_iterator& operator++() {
            if (heapMode_)
                ++mapIt_;
            else
                ++ptr_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const const_iterator& o) const {
            if (heapMode_ != o.heapMode_)
                return false;
            return heapMode_ ? (mapIt_ == o.mapIt_) : (ptr_ == o.ptr_);
        }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }
    };

    // ===== Constructors & Assignment =====
    SmallMap() = default;

    ~SmallMap() = default; // inline_[] and heap_ have proper destructors

    SmallMap(const SmallMap& other) : inlineSize_(other.inlineSize_), useHeap_(other.useHeap_) {
        if (useHeap_) {
            heap_ = std::make_unique<MapType>(*other.heap_);
        } else {
            for (uint8_t i = 0; i < inlineSize_; ++i) {
                inline_[i] = other.inline_[i];
            }
        }
    }

    SmallMap(SmallMap&& other) noexcept : inlineSize_(other.inlineSize_), useHeap_(other.useHeap_) {
        if (useHeap_) {
            heap_ = std::move(other.heap_);
        } else {
            for (uint8_t i = 0; i < inlineSize_; ++i) {
                inline_[i] = std::move(other.inline_[i]);
            }
        }
        other.inlineSize_ = 0;
        other.useHeap_ = false;
    }

    SmallMap& operator=(const SmallMap& other) {
        if (this == &other)
            return *this;
        clear();
        useHeap_ = other.useHeap_;
        inlineSize_ = other.inlineSize_;
        if (useHeap_) {
            heap_ = std::make_unique<MapType>(*other.heap_);
        } else {
            for (uint8_t i = 0; i < inlineSize_; ++i) {
                inline_[i] = other.inline_[i];
            }
        }
        return *this;
    }

    SmallMap& operator=(SmallMap&& other) noexcept {
        if (this == &other)
            return *this;
        clear();
        useHeap_ = other.useHeap_;
        inlineSize_ = other.inlineSize_;
        if (useHeap_) {
            heap_ = std::move(other.heap_);
        } else {
            for (uint8_t i = 0; i < inlineSize_; ++i) {
                inline_[i] = std::move(other.inline_[i]);
            }
        }
        other.inlineSize_ = 0;
        other.useHeap_ = false;
        return *this;
    }

    /// 从 std::unordered_map 赋值（用于 restoreLocalVariables）
    SmallMap& operator=(const MapType& map) {
        clear();
        if (map.size() <= N) {
            // 适合内联存储
            for (const auto& kv : map) {
                inline_[inlineSize_].first = kv.first;
                inline_[inlineSize_].second = kv.second;
                ++inlineSize_;
            }
        } else {
            // 使用堆存储
            useHeap_ = true;
            heap_ = std::make_unique<MapType>(map);
        }
        return *this;
    }

    // ===== Core API =====

    iterator find(const K& key) {
        if (useHeap_) {
            auto it = heap_->find(key);
            return iterator(it);
        }
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            if (inline_[i].first == key) {
                return iterator(&inline_[i]);
            }
        }
        return end();
    }

    const_iterator find(const K& key) const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return const_iterator(m.find(key));
        }
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            if (inline_[i].first == key) {
                return const_iterator(&inline_[i]);
            }
        }
        return end();
    }

    template <typename... Args> std::pair<iterator, bool> try_emplace(const K& key, Args&&... args) {
        if (useHeap_) {
            auto [it, inserted] = heap_->try_emplace(key, std::forward<Args>(args)...);
            return {iterator(it), inserted};
        }
        // 内联模式：先查找是否已存在
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            if (inline_[i].first == key) {
                return {iterator(&inline_[i]), false};
            }
        }
        // 不存在，需要插入
        if (inlineSize_ < N) {
            // 内联空间足够
            inline_[inlineSize_].first = key;
            inline_[inlineSize_].second = V(std::forward<Args>(args)...);
            auto it = iterator(&inline_[inlineSize_]);
            ++inlineSize_;
            return {it, true};
        }
        // 内联空间满，迁移到堆
        promoteToHeap();
        auto [it, inserted] = heap_->try_emplace(key, std::forward<Args>(args)...);
        return {iterator(it), inserted};
    }

    V& operator[](const K& key) {
        if (useHeap_) {
            return (*heap_)[key];
        }
        // 内联模式：查找
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            if (inline_[i].first == key) {
                return inline_[i].second;
            }
        }
        // 不存在，插入默认值
        if (inlineSize_ < N) {
            inline_[inlineSize_].first = key;
            inline_[inlineSize_].second = V{};
            return inline_[inlineSize_++].second;
        }
        // 满了，迁移到堆
        promoteToHeap();
        return (*heap_)[key];
    }

    // ===== Iterators =====

    iterator begin() {
        if (useHeap_)
            return iterator(heap_->begin());
        return iterator(&inline_[0]);
    }
    iterator end() {
        if (useHeap_)
            return iterator(heap_->end());
        return iterator(&inline_[inlineSize_]);
    }
    const_iterator begin() const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return const_iterator(m.begin());
        }
        return const_iterator(&inline_[0]);
    }
    const_iterator end() const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return const_iterator(m.end());
        }
        return const_iterator(&inline_[inlineSize_]);
    }

    // ===== Size & State =====

    size_type size() const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return m.size();
        }
        return static_cast<size_type>(inlineSize_);
    }

    bool empty() const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return m.empty();
        }
        return inlineSize_ == 0;
    }

    void clear() {
        if (useHeap_) {
            heap_.reset();
            useHeap_ = false;
        }
        // 重置内联条目（调用析构/重置为默认状态）
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            inline_[i].first = K{};
            inline_[i].second = V{};
        }
        inlineSize_ = 0;
    }

    size_type erase(const K& key) {
        if (useHeap_) {
            return heap_->erase(key);
        }
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            if (inline_[i].first == key) {
                // 将最后一个条目移到被删除的位置（O(1) 删除，不保序）
                --inlineSize_;
                if (i != inlineSize_) {
                    inline_[i] = std::move(inline_[inlineSize_]);
                }
                inline_[inlineSize_].first = K{};
                inline_[inlineSize_].second = V{};
                return 1;
            }
        }
        return 0;
    }

    size_type count(const K& key) const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return m.count(key);
        }
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            if (inline_[i].first == key)
                return 1;
        }
        return 0;
    }

    /// 转换为 std::unordered_map（用于 snapshotLocalVariables 等需要返回 map 的场景）
    MapType toUnorderedMap() const {
        if (useHeap_) {
            const MapType& m = *heap_;
            return m;
        }
        MapType result;
        result.reserve(inlineSize_);
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            result.emplace(inline_[i].first, inline_[i].second);
        }
        return result;
    }

    /// AUDIT-R4 BUG-11 fix: 强制晋升到堆存储（unordered_map）。
    /// 晋升后通过 find/operator[] 获取的 V* 引用在后续插入下保持稳定
    /// （unordered_map 插入/rehash 不失效引用，仅 erase/clear 失效）。
    /// 用于 Environment::bindInstance 缓存 boundInstance_ 指针前调用，
    /// 消除内联模式 promoteToHeap 搬迁条目导致的悬垂指针风险。
    /// 注意：晋升会使此前获取的内联模式指针/迭代器失效，
    /// 调用方须在晋升后重新定位。
    void ensureHeapStorage() {
        if (!useHeap_)
            promoteToHeap();
    }

private:
    /// 将所有内联条目迁移到堆 unordered_map
    void promoteToHeap() {
        heap_ = std::make_unique<MapType>();
        heap_->reserve(N * 2);
        for (uint8_t i = 0; i < inlineSize_; ++i) {
            heap_->emplace(std::move(inline_[i].first), std::move(inline_[i].second));
            inline_[i].first = K{};
            inline_[i].second = V{};
        }
        inlineSize_ = 0;
        useHeap_ = true;
    }

    value_type inline_[N];
    uint8_t inlineSize_ = 0;
    bool useHeap_ = false;
    std::unique_ptr<MapType> heap_;
};
