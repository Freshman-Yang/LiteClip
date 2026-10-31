#include "clipboard/OfficePolicy.h"
#include "clipboard/HtmlFormatProcessor.h"
#include "platform/GlobalMemory.h"

#include <cstring>
#include <cwctype>
#include <filesystem>
#include <oleidl.h>
#include <string>

PlainTextMetrics AnalyzePlainTextMetrics(std::wstring_view text) noexcept {
    PlainTextMetrics m{};
    if (text.empty()) {
        return m;
    }

    bool lineHasNonSpace = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n') {
            if (ch == L'\r' && (i + 1) < text.size() && text[i + 1] == L'\n') {
                ++i; // 将 \r\n 合并视为一次段落换行
            }
            ++m.totalLines;
            if (!lineHasNonSpace) {
                ++m.emptyLines;
            }
            lineHasNonSpace = false;
        } else if (!std::iswspace(static_cast<wint_t>(ch))) {
            lineHasNonSpace = true;
        }
    }

    // 计入末尾未以换行符结束的最后一段
    if (lineHasNonSpace) {
        ++m.totalLines;
    }
    return m;
}

uint64_t SniffWordDocumentSizeFromClipboard(UINT cfObjectDescriptor, UINT cfObjectLink) noexcept {
    auto extractDocSizeFromWidePath = [](std::wstring_view rawPath) noexcept -> uint64_t {
        size_t pos = 0;
        while (pos < rawPath.size()) {
            // 定位盘符 "X:\" 或 "X:/"
            if (pos + 2 < rawPath.size() &&
                ((rawPath[pos] >= L'A' && rawPath[pos] <= L'Z') || (rawPath[pos] >= L'a' && rawPath[pos] <= L'z')) &&
                rawPath[pos + 1] == L':' &&
                (rawPath[pos + 2] == L'\\' || rawPath[pos + 2] == L'/')) {
                size_t end = pos + 3;
                while (end < rawPath.size() && rawPath[end] >= 0x20 &&
                       rawPath[end] != L'"' && rawPath[end] != L'<' &&
                       rawPath[end] != L'>' && rawPath[end] != L'|' &&
                       rawPath[end] != L'!' && rawPath[end] != L'#') {
                    ++end;
                }
                std::wstring candidate(rawPath.substr(pos, end - pos));
                while (!candidate.empty() && (candidate.back() == L' ' || candidate.back() == L'.')) {
                    candidate.pop_back();
                }
                if (candidate.size() > 4) {
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (::GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | static_cast<uint64_t>(fad.nFileSizeLow);
                    }
                }
                pos = end;
            } else {
                ++pos;
            }
        }
        return 0;
    };

    // 1. 优先读取 Object Descriptor (OBJECTDESCRIPTOR 结构体，dwSrcOfCopy 偏移处存放 UTF-16 源文件路径+书签名)
    if (cfObjectDescriptor != 0 && ::IsClipboardFormatAvailable(cfObjectDescriptor)) {
        if (HANDLE hObj = ::GetClipboardData(cfObjectDescriptor)) {
            const SIZE_T rawSize = ::GlobalSize(hObj);
            if (rawSize >= sizeof(OBJECTDESCRIPTOR) && rawSize <= 65536) {
                ScopedGlobalLock lock(hObj);
                if (lock) {
                    const auto* od = static_cast<const OBJECTDESCRIPTOR*>(lock.Get());
                    if (od->dwSrcOfCopy >= sizeof(OBJECTDESCRIPTOR) && od->dwSrcOfCopy + sizeof(wchar_t) <= rawSize) {
                        const auto* srcStr = reinterpret_cast<const wchar_t*>(
                            static_cast<const uint8_t*>(lock.Get()) + od->dwSrcOfCopy);
                        const size_t maxWChars = (rawSize - od->dwSrcOfCopy) / sizeof(wchar_t);
                        size_t wlen = 0;
                        while (wlen < maxWChars && srcStr[wlen] != L'\0') {
                            ++wlen;
                        }
                        if (uint64_t fsize = extractDocSizeFromWidePath(std::wstring_view(srcStr, wlen)); fsize > 0) {
                            return fsize;
                        }
                    }
                }
            }
        }
    }

    // 2. 备选读取 ObjectLink (ANSI 双零结尾字符串列表: "Word.Document.12\0D:\...\test.docx\0!OLE_LINK1\0\0")
    if (cfObjectLink != 0 && ::IsClipboardFormatAvailable(cfObjectLink)) {
        if (HANDLE hLink = ::GetClipboardData(cfObjectLink)) {
            const SIZE_T rawSize = ::GlobalSize(hLink);
            if (rawSize > 4 && rawSize <= 65536) {
                ScopedGlobalLock lock(hLink);
                if (lock) {
                    const auto* base = static_cast<const char*>(lock.Get());
                    size_t offset = 0;
                    while (offset < rawSize && base[offset] != '\0') {
                        const size_t maxRem = rawSize - offset;
                        const void* nullPtr = std::memchr(base + offset, '\0', maxRem);
                        const size_t len = nullPtr
                            ? static_cast<size_t>(static_cast<const char*>(nullPtr) - (base + offset))
                            : maxRem;
                        if (len > 3) {
                            std::wstring wideStr = Utf8OrAcpToWide(std::string_view(base + offset, len));
                            if (uint64_t fsize = extractDocSizeFromWidePath(wideStr); fsize > 0) {
                                return fsize;
                            }
                        }
                        offset += len + 1;
                    }
                }
            }
        }
    }

    return 0;
}

bool EvaluateHtmlForSmallTier(const std::vector<uint8_t>& htmlBytes) {
    if (htmlBytes.empty()) {
        return true;
    }
    // 1. 若原始 HTML 超过 2 MB（例如 15~50 张图的 HTML 高达 3~8 MB），0 耗时直接判定为中型，绝不碰 RTF！
    if (htmlBytes.size() > TierLimits::kSmallMaxHtmlBytes) {
        return false;
    }

    const std::string_view html(reinterpret_cast<const char*>(htmlBytes.data()), htmlBytes.size());

    if (FindIgnoreCase(html, "<o:ole") != std::string_view::npos) {
        return false;
    }

    const std::filesystem::path baseDir = DetectHtmlBaseDir(html);
    size_t imageCount = 0;
    uint64_t totalImageBytes = 0;

    // 2. 直接通过目录遍历极速统计 Word 临时文件夹下的图片数量与总大小（耗时 < 0.2ms）
    if (!baseDir.empty()) {
        std::wstring searchPattern = baseDir.lexically_normal().wstring();
        if (!searchPattern.empty() && searchPattern.back() != L'\\') {
            searchPattern.push_back(L'\\');
        }
        searchPattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = ::FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &fd,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (hFind != INVALID_HANDLE_VALUE) {
            bool withinSmallLimit = true;
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    std::wstring lowerName = fd.cFileName;
                    for (wchar_t& ch : lowerName) ch = static_cast<wchar_t>(std::towlower(ch));
                    if (!IsWordAuxiliaryHtmlFile(lowerName)) {
                        const uint64_t fsize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | static_cast<uint64_t>(fd.nFileSizeLow);
                        ++imageCount;
                        totalImageBytes += fsize;
                        if (imageCount > TierLimits::kSmallMaxImageCount ||
                            totalImageBytes > TierLimits::kSmallMaxTotalImgBytes) {
                            withinSmallLimit = false;
                            break;
                        }
                    }
                }
            } while (::FindNextFileW(hFind, &fd));
            ::FindClose(hFind);
            if (!withinSmallLimit) {
                return false;
            }
            if (imageCount > 0) {
                return true;
            }
        }
    }

    return true;
}
