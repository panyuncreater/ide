// ============================================================
// interpreter/SimdUtils.h — R134 SIMD 向量化基础设施
// ------------------------------------------------------------
// 提供跨平台 SIMD 检测与数值批量运算内核。
//
// 设计目标：
//   1. 教学清晰：AVX2 intrinsics 配详尽注释，展示数据并行编程模式
//   2. 安全退化：无 AVX2 时透明回退到标量实现，三后端语义不变
//   3. 头文件内联：所有内核 inline，避免跨翻译单元调用开销
//
// 支持的内核：
//   - simdSumInt64(data, n)         有符号 64 位整数水平和
//   - simdSumDouble(data, n)        双精度浮点水平和
//   - simdMinInt64(data, n)         有符号 64 位整数最小值
//   - simdMaxInt64(data, n)         有符号 64 位整数最大值
//   - decodeInt48Batch(src, dst, n) NaN-boxed int48 批量解码到 int64_t[]
//
// 编译标志：
//   - CMake MINILANG_ENABLE_SIMD=ON 时定义 MINILANG_HAS_SIMD
//   - MSVC: /arch:AVX2 启用 __AVX2__ 等价的内置宏
//   - GCC/Clang: -mavx2 -mfma 启用 __AVX2__
//   - 若未启用架构标志，仍可包含本头，所有内核自动退化为标量实现
//
// 关键约束：
//   - SIMD 内核假设输入数据 8 字节对齐（int64_t/double 自然对齐）
//   - 元素数 n 无需是 4/8 的倍数，尾部用标量处理
//   - 溢出检查由调用方负责（SIMD 内核不检查 int64 累加溢出）
// ============================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// 七特性 MVP 阶段 5：运行时 cpuid 检测所需头文件。
//   - MSVC：<intrin.h> 提供 __cpuidex
//   - GCC/Clang：<cpuid.h> 提供 __get_cpuid_count（或 __builtin_cpu_supports）
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

// ============================================================
// SIMD 能力检测
// ============================================================
#if defined(__AVX2__)
#define MINILANG_HAS_AVX2 1
#elif defined(_MSC_VER) && defined(__AVX2__)
#define MINILANG_HAS_AVX2 1
#elif defined(MINILANG_FORCE_AVX2)
#define MINILANG_HAS_AVX2 1
#else
#define MINILANG_HAS_AVX2 0
#endif

#if defined(__SSE4_2__) || (defined(_MSC_VER) && defined(__AVX__))
#define MINILANG_HAS_SSE42 1
#else
#define MINILANG_HAS_SSE42 0
#endif

#if MINILANG_HAS_AVX2
#include <immintrin.h>
#elif MINILANG_HAS_SSE42
#include <emmintrin.h>
#include <nmmintrin.h>
#endif

namespace minilang::simd {

// ============================================================
// 配置常量
// ============================================================
// 触发 SIMD 路径的最小元素数。小于此阈值时标量路径更快
// （避免 SIMD 初始化与水平归约开销）。
constexpr size_t SIMD_MIN_ELEMENTS = 16;

// AVX2 寄存器可容纳 4 个 int64/double。SSE2 寄存器可容纳 2 个。
constexpr size_t AVX2_LANES_I64 = 4;
constexpr size_t SSE2_LANES_I64 = 2;

// ============================================================
// simdSumInt64 — 有符号 64 位整数水平和
// ------------------------------------------------------------
// 输入：data 指向 n 个连续的 int64_t（8 字节对齐）
// 输出：data[0] + data[1] + ... + data[n-1]
//
// AVX2 实现：
//   - 每次迭代加载 4 个 int64 到 __m256i 累加器
//   - 循环结束后水平归约 4-lane 累加器到标量
//   - 尾部 (< 4 元素) 标量处理
//
// 注：int64 加法溢出为 UB，调用方需独立做溢出检查。
// ============================================================
inline int64_t simdSumInt64(const int64_t* data, size_t n) {
#if MINILANG_HAS_AVX2
    if (n >= SIMD_MIN_ELEMENTS) {
        __m256i acc = _mm256_setzero_si256();
        size_t i = 0;
        for (; i + AVX2_LANES_I64 <= n; i += AVX2_LANES_I64) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            acc = _mm256_add_epi64(acc, v);
        }
        // 水平归约：acc[0] + acc[1] + acc[2] + acc[3]
        // _mm256_extracti128_si256 提取低/高 128 位
        __m128i lo = _mm256_castsi256_si128(acc);
        __m128i hi = _mm256_extracti128_si256(acc, 1);
        __m128i sum128 = _mm_add_epi64(lo, hi);
        // sum128 = [a0+a2, a1+a3]（按 lane 索引）
        // 再水平加低 64 与高 64
        __m128i shuffle = _mm_shuffle_epi32(sum128, _MM_SHUFFLE(1, 0, 3, 2));
        __m128i final = _mm_add_epi64(sum128, shuffle);
        int64_t result;
        _mm_storel_epi64(reinterpret_cast<__m128i*>(&result), final);
        // 尾部标量
        for (; i < n; ++i)
            result += data[i];
        return result;
    }
#endif
    // 标量回退
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += data[i];
    return sum;
}

// ============================================================
// simdSumDouble — 双精度浮点水平和
// ------------------------------------------------------------
// 输入：data 指向 n 个连续的 double（8 字节对齐）
// 输出：data[0] + data[1] + ... + data[n-1]
//
// AVX2 实现：
//   - 每次迭代加载 4 个 double 到 __m256d 累加器
//   - 水平归约 4-lane 累加器
//   - 尾部标量处理
// ============================================================
inline double simdSumDouble(const double* data, size_t n) {
#if MINILANG_HAS_AVX2
    if (n >= SIMD_MIN_ELEMENTS) {
        __m256d acc = _mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(data + i);
            acc = _mm256_add_pd(acc, v);
        }
        // 水平归约：acc[0] + acc[1] + acc[2] + acc[3]
        __m128d lo = _mm256_castpd256_pd128(acc);
        __m128d hi = _mm256_extractf128_pd(acc, 1);
        __m128d sum128 = _mm_add_pd(lo, hi);
        __m128d shuffle = _mm_shuffle_pd(sum128, sum128, 1);
        __m128d final = _mm_add_pd(sum128, shuffle);
        double result;
        _mm_storel_pd(&result, final);
        for (; i < n; ++i)
            result += data[i];
        return result;
    }
#endif
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += data[i];
    return sum;
}

// ============================================================
// simdMinInt64 / simdMaxInt64 — 整数批量最小/最大值
// ------------------------------------------------------------
// 输入：data 指向 n 个 int64_t（n >= 1）
// 输出：min/max(data[0..n-1])
//
// AVX2 使用 _mm256_min_epi64 / _mm256_max_epi64（要求 AVX2）。
// 注：_mm256_min_epi64 是 AVX512VL 指令，AVX2 不直接支持 64-bit min。
// 变通：用比较 + blend 实现：min = (a < b) ? a : b
// ============================================================
inline int64_t simdMinInt64(const int64_t* data, size_t n) {
    if (n == 0)
        return 0;
#if MINILANG_HAS_AVX2
    if (n >= SIMD_MIN_ELEMENTS) {
        __m256i acc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
        size_t i = AVX2_LANES_I64;
        for (; i + AVX2_LANES_I64 <= n; i += AVX2_LANES_I64) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            // min = (a < b) ? a : b
            // _mm256_cmpgt_epi64(a, b) 返回 a > b 的 mask
            // 若 a > b，则取 b；否则取 a
            __m256i mask = _mm256_cmpgt_epi64(acc, v);
            acc = _mm256_blendv_epi8(acc, v, mask);
        }
        // 水平归约：4 lane → 2 lane → 标量
        __m128i lo = _mm256_castsi256_si128(acc);
        __m128i hi = _mm256_extracti128_si256(acc, 1);
        __m128i cmp = _mm_cmpgt_epi64(lo, hi);
        __m128i m = _mm_blendv_epi8(lo, hi, cmp);
        // m 含 2 个 int64，取最小者：高 64 移到低 64 后比较
        int64_t lo64;
        _mm_storel_epi64(reinterpret_cast<__m128i*>(&lo64), m);
        __m128i high64 = _mm_srli_si128(m, 8); // 右移 8 字节，高 64 → 低 64
        int64_t hi64;
        _mm_storel_epi64(reinterpret_cast<__m128i*>(&hi64), high64);
        int64_t result = (lo64 < hi64) ? lo64 : hi64;
        // 尾部
        for (; i < n; ++i)
            if (data[i] < result)
                result = data[i];
        return result;
    }
#endif
    int64_t m = data[0];
    for (size_t i = 1; i < n; ++i)
        if (data[i] < m)
            m = data[i];
    return m;
}

inline int64_t simdMaxInt64(const int64_t* data, size_t n) {
    if (n == 0)
        return 0;
#if MINILANG_HAS_AVX2
    if (n >= SIMD_MIN_ELEMENTS) {
        __m256i acc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
        size_t i = AVX2_LANES_I64;
        for (; i + AVX2_LANES_I64 <= n; i += AVX2_LANES_I64) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            // max = (a > b) ? a : b
            __m256i mask = _mm256_cmpgt_epi64(v, acc);
            acc = _mm256_blendv_epi8(acc, v, mask);
        }
        __m128i lo = _mm256_castsi256_si128(acc);
        __m128i hi = _mm256_extracti128_si256(acc, 1);
        __m128i cmp = _mm_cmpgt_epi64(hi, lo);
        __m128i m = _mm_blendv_epi8(lo, hi, cmp);
        int64_t lo64;
        _mm_storel_epi64(reinterpret_cast<__m128i*>(&lo64), m);
        __m128i high64 = _mm_srli_si128(m, 8);
        int64_t hi64;
        _mm_storel_epi64(reinterpret_cast<__m128i*>(&hi64), high64);
        int64_t result = (lo64 > hi64) ? lo64 : hi64;
        for (; i < n; ++i)
            if (data[i] > result)
                result = data[i];
        return result;
    }
#endif
    int64_t m = data[0];
    for (size_t i = 1; i < n; ++i)
        if (data[i] > m)
            m = data[i];
    return m;
}

// ============================================================
// simdMinDouble / simdMaxDouble — 双精度浮点批量最小/最大值
// ------------------------------------------------------------
// 七特性 MVP 阶段 5：补齐 double 版 min/max 内核（与 int64 版对称）。
// 输入：data 指向 n 个 double（n >= 1）；输出：min/max(data[0..n-1])。
// AVX2 使用 _mm256_min_pd / _mm256_max_pd（原生 256-bit 双精度比较）。
// NaN 处理：_mm256_min_pd/_mm256_max_pd 的 NaN 行为与标量 `<`/`>` 对齐
//（任一操作数为 NaN 时返回第二操作数）——与标量回退路径的 `if (x < m)`
// 语义存在细微差异，但 sum/min/max 内建仅在全部元素为数值时走快路径，
// 含 NaN 的数组由调用方（BuiltinMethods）才会回退，此处不需严格对齐 NaN。
// ============================================================
inline double simdMinDouble(const double* data, size_t n) {
    if (n == 0)
        return 0.0;
#if MINILANG_HAS_AVX2
    if (n >= SIMD_MIN_ELEMENTS) {
        __m256d acc = _mm256_loadu_pd(data);
        size_t i = 4;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(data + i);
            acc = _mm256_min_pd(acc, v);
        }
        // 水平归约：4 lane → 2 lane → 标量
        __m128d lo = _mm256_castpd256_pd128(acc);
        __m128d hi = _mm256_extractf128_pd(acc, 1);
        __m128d m2 = _mm_min_pd(lo, hi);
        __m128d shuffle = _mm_shuffle_pd(m2, m2, 1);
        __m128d m1 = _mm_min_pd(m2, shuffle);
        double result;
        _mm_storel_pd(&result, m1);
        for (; i < n; ++i)
            if (data[i] < result)
                result = data[i];
        return result;
    }
#endif
    double m = data[0];
    for (size_t i = 1; i < n; ++i)
        if (data[i] < m)
            m = data[i];
    return m;
}

inline double simdMaxDouble(const double* data, size_t n) {
    if (n == 0)
        return 0.0;
#if MINILANG_HAS_AVX2
    if (n >= SIMD_MIN_ELEMENTS) {
        __m256d acc = _mm256_loadu_pd(data);
        size_t i = 4;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(data + i);
            acc = _mm256_max_pd(acc, v);
        }
        __m128d lo = _mm256_castpd256_pd128(acc);
        __m128d hi = _mm256_extractf128_pd(acc, 1);
        __m128d m2 = _mm_max_pd(lo, hi);
        __m128d shuffle = _mm_shuffle_pd(m2, m2, 1);
        __m128d m1 = _mm_max_pd(m2, shuffle);
        double result;
        _mm_storel_pd(&result, m1);
        for (; i < n; ++i)
            if (data[i] > result)
                result = data[i];
        return result;
    }
#endif
    double m = data[0];
    for (size_t i = 1; i < n; ++i)
        if (data[i] > m)
            m = data[i];
    return m;
}

// ============================================================
// decodeInt48Batch — NaN-boxed int48 批量解码
// ------------------------------------------------------------
// 输入：src 指向 n 个 Value（每个 8 字节，NaN-boxed int48 编码）
//       调用方必须保证所有 src[i].isInt() == true
// 输出：dst 指向 n 个 int64_t，存放符号扩展后的整数值
//
// 解码逻辑（与 NaNBox::asInt 一致）：
//   payload = bits & INT48_MASK           // 清除高 16 位 tag
//   if (payload & INT47_SIGN_BIT)         // bit 47 为符号位
//       payload |= ~INT48_MASK            // 负数：高 16 位填 1
//   return (int64_t)payload
//
// 本实现采用标量循环（不直接 SIMD 化解码本身）：
//   - 解码需要分支判断（每元素的符号位不同），SIMD 化收益有限
//   - 解码后的 int64_t[] 数组连续无 tag，可被后续 SIMD 内核高效处理
//   - 教学价值：展示 NaN-boxing 与 SIMD 数据布局的差异与桥接策略
//
// 性能注：解码开销 O(n)，SIMD 累加 O(n/4)。整体复杂度仍为 O(n)，
// 但常数因子小于纯标量累加（4 路并行 + 减少分支预测失败）。
// ============================================================
inline void decodeInt48Batch(const uint64_t* srcBits, int64_t* dst, size_t n) {
    constexpr uint64_t INT48_MASK = 0x0000FFFFFFFFFFFFULL;
    constexpr uint64_t INT47_SIGN_BIT = 0x0000800000000000ULL;
    constexpr uint64_t HIGH16_ONES = 0xFFFF000000000000ULL;
    for (size_t i = 0; i < n; ++i) {
        uint64_t payload = srcBits[i] & INT48_MASK;
        if (payload & INT47_SIGN_BIT) {
            payload |= HIGH16_ONES;
        }
        dst[i] = static_cast<int64_t>(payload);
    }
}

// ============================================================
// hasAvx2Support — 运行时 CPU AVX2 能力检测
// ------------------------------------------------------------
// 七特性 MVP 阶段 5：从编译期常量升级为运行时 cpuid 检测。
//   - 未启用 /arch:AVX2（MINILANG_HAS_AVX2==0）：内核未编出 AVX2 指令，
//     检测直接短路返回 false（即使 CPU 支持也不能执行未编出的指令）。
//   - 启用 /arch:AVX2：首次调用时 cpuid 探测（leaf 7, EBX bit 5 = AVX2），
//     static 局部变量缓存结果（C++11 保证线程安全初始化）。
// 注：MINILANG_HAS_AVX2==1 时编译器已假设目标 CPU 支持 AVX2（/arch:AVX2 启用
// 全局 AVX2 代码生成），运行时检测仅作为防御性确认（在不支持 AVX2 的
// 旧 CPU 上运行会直接非法指令崩溃，与其他全局 AVX2 代码行为一致）。
// ============================================================
inline bool hasAvx2Support() {
#if MINILANG_HAS_AVX2
    // 首次调用时运行 cpuid，结果缓存于 static（一次性探测）
    static const bool supported = []() -> bool {
#if defined(_MSC_VER)
        int regs[4] = {0, 0, 0, 0};
        // leaf 7, subleaf 0：EBX bit 5 = AVX2
        __cpuidex(regs, 7, 0);
        return (regs[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
        return __builtin_cpu_supports("avx2") != 0;
#else
        return true; // 编译器已启用 AVX2 但无 cpuid 接口，保守信任编译期判定
#endif
    }();
    return supported;
#else
    // 未启用 /arch:AVX2：内核未编出 AVX2 指令，无论 CPU 如何都走标量路径。
    return false;
#endif
}

} // namespace minilang::simd
