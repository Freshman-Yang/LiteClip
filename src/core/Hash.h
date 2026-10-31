#pragma once

#include "core/Types.h"
#include <cstddef>
#include <cstdint>
#include <limits>

inline constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;

// WIC 内存缓冲区长度参数为 UINT/DWORD，转换前必须能够无损表示。
inline constexpr bool FitsWicBuffer(size_t size) noexcept {
    return size > 0 && size <= std::numeric_limits<UINT>::max();
}

// FNV-1a 64位候选哈希；大内容抽样，去重时仍需验证完整内容。
inline uint64_t ComputeFnv1aHash(const void* data, size_t size, uint64_t seed = kFnv1aOffsetBasis) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed ^ static_cast<uint64_t>(size);
    hash *= 1099511628211ULL;

    constexpr size_t kFullHashThreshold = 2ULL * 1024ULL * 1024ULL; // 2 MB 以内全量哈希
    constexpr size_t kSampleChunkSize   = 256ULL * 1024ULL;         // 超大文件抽样块大小 256 KB

    if (size <= kFullHashThreshold) {
        for (size_t i = 0; i < size; ++i) {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= 1099511628211ULL;
        }
    } else {
        // 抽取头部、中部、尾部各 256KB 进行特征哈希，兼顾去重精度与 O(1) 极速性能
        const size_t offsets[3] = { 0, (size - kSampleChunkSize) / 2, size - kSampleChunkSize };
        for (size_t offset : offsets) {
            for (size_t i = 0; i < kSampleChunkSize; ++i) {
                hash ^= static_cast<uint64_t>(bytes[offset + i]);
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}
