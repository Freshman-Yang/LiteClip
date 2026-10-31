#include "platform/FileUtils.h"

#include <algorithm>
#include <cstring>
#include <limits>

bool IsSafeFileName(const std::wstring& fileName) {
    if (fileName.empty() || fileName == L"." || fileName == L"..") {
        return false;
    }
    if (fileName.find(L"..") != std::wstring::npos) {
        return false;
    }
    for (wchar_t ch : fileName) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'\0' ||
            ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' ||
            ch == L'?' || ch == L'*' || ch < 32) {
            return false;
        }
    }
    return true;
}

bool WriteBinaryFile(const std::filesystem::path& filePath, const void* data, size_t sizeInBytes) {
    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    if (sizeInBytes > 0 && data != nullptr) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(sizeInBytes));
    }
    ofs.flush();
    return ofs.good();
}

bool ReadBinaryFile(const std::filesystem::path& filePath, std::vector<uint8_t>& outBytes) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        return false;
    }

    const uintmax_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec || fileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
        fileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    try {
        outBytes.resize(static_cast<size_t>(fileSize));
    } catch (...) {
        return false;
    }

    if (fileSize > 0) {
        ifs.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(fileSize));
        if (!ifs.good() && !ifs.eof()) {
            outBytes.clear();
            return false;
        }
        if (static_cast<uintmax_t>(ifs.gcount()) != fileSize) {
            outBytes.clear();
            return false;
        }
    }
    return true;
}

bool ReadUtf16File(const std::filesystem::path& filePath, std::wstring& outText) {
    outText.clear();
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(filePath, ec);
    if (ec || size % sizeof(wchar_t) != 0 ||
        size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) return false;
    try {
        outText.resize(static_cast<size_t>(size / sizeof(wchar_t)));
    } catch (...) {
        return false;
    }
    if (size > 0) {
        ifs.read(reinterpret_cast<char*>(outText.data()), static_cast<std::streamsize>(size));
        if (static_cast<uintmax_t>(ifs.gcount()) != size) {
            outText.clear();
            return false;
        }
    }
    return true;
}

bool FileEqualsBytes(const std::filesystem::path& path, const void* data, size_t size) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != size || ec) return false;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    char buffer[64 * 1024];
    const auto* bytes = static_cast<const char*>(data);
    for (size_t offset = 0; offset < size;) {
        const size_t count = std::min(sizeof(buffer), size - offset);
        ifs.read(buffer, static_cast<std::streamsize>(count));
        if (ifs.gcount() != static_cast<std::streamsize>(count) ||
            std::memcmp(buffer, bytes + offset, count) != 0) return false;
        offset += count;
    }
    return true;
}

bool WriteWString(std::ofstream& ofs, const std::wstring& str) {
    uint32_t len = static_cast<uint32_t>(std::min<size_t>(str.size(), kMaxStringChars));
    // 防止在最大字符截断处劈裂 UTF-16 代理对
    if (len > 0 && len < str.size() && IsHighSurrogate(str[len - 1])) {
        --len;
    }
    if (!WritePod(ofs, len)) {
        return false;
    }
    if (len > 0) {
        ofs.write(reinterpret_cast<const char*>(str.data()), static_cast<std::streamsize>(len * sizeof(wchar_t)));
    }
    return ofs.good();
}

bool ReadWString(std::ifstream& ifs, std::wstring& outStr) {
    uint32_t len = 0;
    if (!ReadPod(ifs, len)) {
        return false;
    }
    if (len > kMaxStringChars) {
        return false;
    }
    outStr.resize(len);
    if (len > 0) {
        const std::streamsize byteCount = static_cast<std::streamsize>(len * sizeof(wchar_t));
        ifs.read(reinterpret_cast<char*>(outStr.data()), byteCount);
        if (!ifs.good() || ifs.gcount() != byteCount) {
            outStr.clear();
            return false;
        }
    }
    return true;
}
