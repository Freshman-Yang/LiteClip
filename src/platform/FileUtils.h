#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

inline constexpr uint32_t kMaxStringChars = 4096;

inline constexpr bool IsHighSurrogate(wchar_t ch) noexcept {
    return ch >= 0xD800 && ch <= 0xDBFF;
}

bool IsSafeFileName(const std::wstring& fileName);

bool WriteBinaryFile(const std::filesystem::path& filePath, const void* data, size_t sizeInBytes);
bool ReadBinaryFile(const std::filesystem::path& filePath, std::vector<uint8_t>& outBytes);
bool ReadUtf16File(const std::filesystem::path& filePath, std::wstring& outText);

// 抽样哈希只用于筛选；命中后分块验证，避免误合并未抽样区域不同的大内容。
bool FileEqualsBytes(const std::filesystem::path& path, const void* data, size_t size);

template <typename T>
bool WritePod(std::ofstream& ofs, const T& value) {
    ofs.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return ofs.good();
}

template <typename T>
bool ReadPod(std::ifstream& ifs, T& outValue) {
    ifs.read(reinterpret_cast<char*>(&outValue), sizeof(T));
    return ifs.good() && static_cast<size_t>(ifs.gcount()) == sizeof(T);
}

bool WriteWString(std::ofstream& ofs, const std::wstring& str);
bool ReadWString(std::ifstream& ifs, std::wstring& outStr);
