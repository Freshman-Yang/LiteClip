#include "clipboard/HtmlFormatProcessor.h"
#include "platform/Win32Common.h"
#include "core/Hash.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <unordered_map>

// 跨调用复用的扫描缓存：保存 src / href 下一次命中位置，避免某关键字稀少时每次调用都扫描到文档末尾（O(N^2)）
struct HtmlUriScanCache {
    bool initialized = false;
    bool srcDone = false;
    bool hrefDone = false;
    size_t srcPos = std::string_view::npos;
    size_t hrefPos = std::string_view::npos;
};

bool IsAllWhitespace(std::wstring_view str) noexcept {
    if (str.empty()) {
        return true;
    }
    for (wchar_t ch : str) {
        if (!std::iswspace(static_cast<wint_t>(ch))) {
            return false;
        }
    }
    return true;
}

bool DoesTextFillSingleCard(std::wstring_view text) noexcept {
    // 1. 先剔除首尾空白符（零内存分配）
    size_t first = 0;
    while (first < text.size() && std::iswspace(static_cast<wint_t>(text[first]))) {
        ++first;
    }
    if (first >= text.size()) {
        return false;
    }

    size_t last = text.size() - 1;
    while (last > first && std::iswspace(static_cast<wint_t>(text[last]))) {
        --last;
    }

    // 2. 1 倍高度卡片（内容区宽约 288 DIP，高 60 DIP，13 DIP 字号 / 20 DIP 行高）固定可容纳 3 行文本，
    //    单行约容纳 22 个全角字符（44 个半角宽度单位）。
    //    折叠连续空行并统计折行后的行数：若达到 3 行（含显式换行满 3 行，或自动折行深入第 3 行），即视为填满 1 倍高度。
    constexpr uint32_t kLineCapacityUnits = 44U;
    constexpr uint32_t kThirdLineFillUnits = 12U;

    uint32_t lineCount = 1U;
    uint32_t lineUnits = 0U;
    uint32_t explicitNewlines = 0U;
    bool pendingNewline = false;
    bool pendingSpace = false;

    for (size_t i = first; i <= last; ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n') {
            pendingNewline = true;
            pendingSpace = false;
            continue;
        }
        if (std::iswspace(static_cast<wint_t>(ch))) {
            if (!pendingNewline) {
                pendingSpace = true;
            }
            continue;
        }

        if (pendingNewline) {
            ++lineCount;
            ++explicitNewlines;
            lineUnits = 0U;
            pendingNewline = false;
            pendingSpace = false;
        } else if (pendingSpace) {
            pendingSpace = false;
            if (lineUnits + 1U > kLineCapacityUnits) {
                ++lineCount;
                lineUnits = 1U;
            } else {
                lineUnits += 1U;
            }
        }

        const uint32_t charUnits = (ch <= 0x00FFU) ? 1U : 2U;
        if (lineUnits + charUnits > kLineCapacityUnits) {
            ++lineCount;
            lineUnits = charUnits;
        } else {
            lineUnits += charUnits;
        }

        if (lineCount > 3U || (lineCount == 3U && (explicitNewlines >= 2U || lineUnits >= kThirdLineFillUnits))) {
            return true;
        }
    }

    return false;
}

int HexVal(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool StartsWithIgnoreCase(std::string_view str, std::string_view prefix) noexcept {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        unsigned char c1 = static_cast<unsigned char>(str[i]);
        unsigned char c2 = static_cast<unsigned char>(prefix[i]);
        if (c1 >= 'A' && c1 <= 'Z') c1 += ('a' - 'A');
        if (c2 >= 'A' && c2 <= 'Z') c2 += ('a' - 'A');
        if (c1 != c2) return false;
    }
    return true;
}

// 极速大小写无关子串查找：利用首字符大小写双路 memchr 跳过 99% 无关字节，避免在数十 MB 的 Word HTML 中 O(N*M) 卡死
size_t FindIgnoreCase(std::string_view haystack, std::string_view needle, size_t startPos) noexcept {
    if (needle.empty() || startPos >= haystack.size() || haystack.size() - startPos < needle.size()) {
        return std::string_view::npos;
    }

    const char firstChar = needle[0];
    const char firstLower = (firstChar >= 'A' && firstChar <= 'Z') ? static_cast<char>(firstChar + ('a' - 'A')) : firstChar;
    const char firstUpper = (firstChar >= 'a' && firstChar <= 'z') ? static_cast<char>(firstChar - ('a' - 'A')) : firstChar;

    const char* const base = haystack.data();
    const size_t limit = haystack.size() - needle.size();
    size_t pos = startPos;

    if (firstLower == firstUpper) {
        while (pos <= limit) {
            const void* p = std::memchr(base + pos, static_cast<unsigned char>(firstLower), limit - pos + 1);
            if (!p) return std::string_view::npos;
            const size_t idx = static_cast<size_t>(static_cast<const char*>(p) - base);
            if (StartsWithIgnoreCase(haystack.substr(idx, needle.size()), needle)) {
                return idx;
            }
            pos = idx + 1;
        }
    } else {
        // 缓存两路 memchr 的下一个命中位置：仅当缓存位置已被越过时才重新扫描，
        // 避免某一路（如大写 'H'）在数十 MB 文本中从不出现时，每轮迭代都全量扫描到末尾造成 O(N^2) 卡死
        const char* nextLow = nullptr;
        const char* nextUp  = nullptr;
        bool lowExhausted = false;
        bool upExhausted  = false;
        while (pos <= limit) {
            const size_t rem = limit - pos + 1;
            if (!lowExhausted && (nextLow == nullptr || nextLow < base + pos)) {
                nextLow = static_cast<const char*>(std::memchr(base + pos, static_cast<unsigned char>(firstLower), rem));
                if (!nextLow) lowExhausted = true;
            }
            if (!upExhausted && (nextUp == nullptr || nextUp < base + pos)) {
                nextUp = static_cast<const char*>(std::memchr(base + pos, static_cast<unsigned char>(firstUpper), rem));
                if (!nextUp) upExhausted = true;
            }

            const char* pLow = lowExhausted ? nullptr : nextLow;
            const char* pUp  = upExhausted  ? nullptr : nextUp;
            if (!pLow && !pUp) return std::string_view::npos;

            const char* pBest = nullptr;
            if (pLow && pUp) {
                pBest = (pLow < pUp) ? pLow : pUp;
            } else {
                pBest = pLow ? pLow : pUp;
            }

            const size_t idx = static_cast<size_t>(pBest - base);
            if (StartsWithIgnoreCase(haystack.substr(idx, needle.size()), needle)) {
                return idx;
            }
            pos = idx + 1;
        }
    }
    return std::string_view::npos;
}

std::wstring Utf8OrAcpToWide(std::string_view str) {
    if (str.empty()) return L"";
    int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), static_cast<int>(str.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (wlen <= 0) {
        codePage = CP_ACP;
        flags = 0;
        wlen = ::MultiByteToWideChar(codePage, flags, str.data(), static_cast<int>(str.size()), nullptr, 0);
    }
    if (wlen <= 0) return L"";
    std::wstring result(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(codePage, flags, str.data(), static_cast<int>(str.size()), result.data(), wlen);
    return result;
}

std::string WideToUtf8(std::wstring_view wideStr) {
    if (wideStr.empty()) {
        return {};
    }
    const int u8Len = ::WideCharToMultiByte(
        CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.size()), nullptr, 0, nullptr, nullptr);
    if (u8Len <= 0) {
        return {};
    }
    std::string u8(static_cast<size_t>(u8Len), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.size()), u8.data(), u8Len, nullptr, nullptr);
    return u8;
}

// 反转义 URI，同时自动剔除 Word HTML 因 80 列折行强行插入到 URI 中间的 \r 和 \n
std::string UrlUnescape(std::string_view uri) {
    std::string unescaped;
    unescaped.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        const char ch = uri[i];
        if (ch == '\r' || ch == '\n') {
            continue; // 忽略 HTML 折行符
        }
        if (ch == '%' && i + 2 < uri.size()) {
            // 处理可能跨行的 %XX
            size_t i1 = i + 1;
            while (i1 < uri.size() && (uri[i1] == '\r' || uri[i1] == '\n')) ++i1;
            size_t i2 = i1 + 1;
            while (i2 < uri.size() && (uri[i2] == '\r' || uri[i2] == '\n')) ++i2;
            if (i2 < uri.size()) {
                const int hi = HexVal(uri[i1]);
                const int lo = HexVal(uri[i2]);
                if (hi >= 0 && lo >= 0) {
                    unescaped.push_back(static_cast<char>((hi << 4) | lo));
                    i = i2;
                    continue;
                }
            }
        }
        unescaped.push_back(ch == '/' ? '\\' : ch);
    }
    return unescaped;
}

// 仅允许本地盘符绝对路径（如 C:\...），拒绝 UNC 网络路径（\\server\share）与设备路径（\\.\、\\?\）
bool IsLocalDrivePath(std::wstring_view path) noexcept {
    return path.size() >= 3 &&
           ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' &&
           (path[2] == L'\\' || path[2] == L'/');
}

// 仅将已知富文本图片及样式表扩展名作为可持久化资产，避免误拷贝无关本地文件
bool IsSupportedHtmlAssetExtension(std::wstring_view lowerFileName) noexcept {
    return lowerFileName.ends_with(L".png") ||
           lowerFileName.ends_with(L".jpg") ||
           lowerFileName.ends_with(L".jpeg") ||
           lowerFileName.ends_with(L".gif") ||
           lowerFileName.ends_with(L".bmp") ||
           lowerFileName.ends_with(L".webp") ||
           lowerFileName.ends_with(L".ico") ||
           lowerFileName.ends_with(L".tif") ||
           lowerFileName.ends_with(L".tiff") ||
           lowerFileName.ends_with(L".dib") ||
           lowerFileName.ends_with(L".emf") ||
           lowerFileName.ends_with(L".wmf") ||
           lowerFileName.ends_with(L".emz") ||
           lowerFileName.ends_with(L".wmz") ||
           lowerFileName.ends_with(L".svg") ||
           lowerFileName.ends_with(L".css");
}

// 判断路径是否位于 Office 剪切板临时目录中
bool IsOfficeTempClipPath(std::wstring_view lowerNormPath) noexcept {
    return lowerNormPath.find(L"\\msohtmlclip") != std::wstring_view::npos ||
           lowerNormPath.find(L"\\msoclip") != std::wstring_view::npos;
}

constexpr uint64_t kMaxHtmlAssetFileBytes = 32ULL * 1024ULL * 1024ULL;

// 支持 file:///C:/...、相对路径，并智能对齐 %TEMP%\msohtmlclip1\01 目录层级
std::wstring ResolveHtmlLocalFilePath(std::string_view rawUri, const std::filesystem::path& baseDir) {
    while (!rawUri.empty() && (rawUri.front() == ' ' || rawUri.front() == '\t' || rawUri.front() == '\r' || rawUri.front() == '\n')) {
        rawUri.remove_prefix(1);
    }
    while (!rawUri.empty() && (rawUri.back() == ' ' || rawUri.back() == '\t' || rawUri.back() == '\r' || rawUri.back() == '\n')) {
        rawUri.remove_suffix(1);
    }
    if (rawUri.empty()) return L"";

    if (StartsWithIgnoreCase(rawUri, "data:") ||
        StartsWithIgnoreCase(rawUri, "http:") ||
        StartsWithIgnoreCase(rawUri, "https:") ||
        StartsWithIgnoreCase(rawUri, "blob:") ||
        StartsWithIgnoreCase(rawUri, "cid:") ||
        StartsWithIgnoreCase(rawUri, "mailto:") ||
        StartsWithIgnoreCase(rawUri, "javascript:") ||
        StartsWithIgnoreCase(rawUri, "about:") ||
        rawUri.starts_with("#") ||
        rawUri.starts_with("//") ||
        rawUri.starts_with("\\\\")) {
        return L"";
    }

    if (StartsWithIgnoreCase(rawUri, "file://")) {
        rawUri.remove_prefix(7);
        if (StartsWithIgnoreCase(rawUri, "localhost/")) {
            rawUri.remove_prefix(10);
        }
        std::string unescaped = UrlUnescape(rawUri);
        std::string_view uView(unescaped);
        while (uView.size() >= 3 && (uView[0] == '\\' || uView[0] == '/') &&
               ((uView[1] >= 'A' && uView[1] <= 'Z') || (uView[1] >= 'a' && uView[1] <= 'z')) &&
               (uView[2] == ':' || uView[2] == '|')) {
            uView.remove_prefix(1);
        }
        std::string cleaned(uView);
        if (cleaned.size() >= 2 && cleaned[1] == '|') {
            cleaned[1] = ':';
        }
        std::wstring widePath = Utf8OrAcpToWide(cleaned);
        if (!widePath.empty()) {
            std::wstring norm = std::filesystem::path(widePath).lexically_normal().wstring();
            if (IsLocalDrivePath(norm)) {
                return norm;
            }
        }
        return L"";
    }

    std::string unescaped = UrlUnescape(rawUri);
    if (unescaped.starts_with("\\\\") || unescaped.starts_with("//")) {
        return L"";
    }
    while (!unescaped.empty() && (unescaped.front() == '\\' || unescaped.front() == '/')) {
        unescaped.erase(unescaped.begin());
    }

    // 绝对盘符路径 (如 C:\...)
    if (unescaped.size() >= 3 &&
        ((unescaped[0] >= 'A' && unescaped[0] <= 'Z') || (unescaped[0] >= 'a' && unescaped[0] <= 'z')) &&
        (unescaped[1] == ':' || unescaped[1] == '|') &&
        (unescaped[2] == '\\' || unescaped[2] == '/')) {
        if (unescaped[1] == '|') {
            unescaped[1] = ':';
        }
        std::wstring widePath = Utf8OrAcpToWide(unescaped);
        if (!widePath.empty()) {
            std::wstring norm = std::filesystem::path(widePath).lexically_normal().wstring();
            if (IsLocalDrivePath(norm)) {
                return norm;
            }
        }
        return L"";
    }

    const std::wstring relWide = Utf8OrAcpToWide(unescaped);
    if (relWide.empty()) {
        return L"";
    }

    // 若相对路径以 msohtmlclip / msoclip 开头，直接拼接在 %TEMP% 根目录下
    if (StartsWithIgnoreCase(unescaped, "msohtmlclip") || StartsWithIgnoreCase(unescaped, "msoclip")) {
        wchar_t tempPath[MAX_PATH + 1]{};
        const DWORD len = ::GetTempPathW(MAX_PATH, tempPath);
        if (len > 0 && len <= MAX_PATH) {
            return (std::filesystem::path(tempPath) / relWide).lexically_normal().wstring();
        }
    }

    if (!baseDir.empty()) {
        std::filesystem::path candidate = (baseDir / relWide).lexically_normal();
        if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate.wstring();
        }
        // 关键修复：若 baseDir 是 ...\msohtmlclip1\01，而 relWide 是 "01\clip_image002.jpg" 或直接文件名，
        // 尝试用纯文件名在 baseDir 下直接定位
        std::filesystem::path fileOnly = std::filesystem::path(relWide).filename();
        if (!fileOnly.empty()) {
            std::filesystem::path directCandidate = (baseDir / fileOnly).lexically_normal();
            if (::GetFileAttributesW(directCandidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return directCandidate.wstring();
            }
            // Word 有时仅给出文件名，而 SourceURL 指向 msohtmlclip 根目录。
            // 只定位本次引用的文件，不枚举 01 中的其他资源。
            const auto wordCandidate = (baseDir / L"01" / fileOnly).lexically_normal();
            if (::GetFileAttributesW(wordCandidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return wordCandidate.wstring();
            }
        }
        return candidate.wstring();
    }

    return L"";
}

// 普通超链接和 <base> 不是附件；只处理实际图像与样式资源。
bool IsHtmlResourceUri(std::string_view html, size_t uriStart) noexcept {
    const size_t tagStart = html.rfind('<', uriStart);
    if (tagStart == std::string_view::npos) return false;
    const size_t tagEnd = html.find('>', tagStart);
    if (tagEnd == std::string_view::npos || tagEnd < uriStart) return false;
    size_t nameEnd = tagStart + 1;
    while (nameEnd < html.size() && html[nameEnd] != ' ' && html[nameEnd] != '\t' &&
           html[nameEnd] != '\r' && html[nameEnd] != '\n' && html[nameEnd] != '>') ++nameEnd;
    const auto name = html.substr(tagStart + 1, nameEnd - tagStart - 1);
    const auto equalsName = [&name](std::string_view expected) {
        return name.size() == expected.size() && StartsWithIgnoreCase(name, expected);
    };
    if (equalsName("img") || equalsName("v:imagedata") || equalsName("v:fill") ||
        equalsName("source") || equalsName("input")) return true;
    if (equalsName("link")) {
        return tagEnd != std::string_view::npos &&
            FindIgnoreCase(html.substr(tagStart, tagEnd - tagStart), "stylesheet") != std::string_view::npos;
    }
    return false;
}

// 辅助函数：在 HTML 中从 searchPos 开始定位下一个合法的 src= 或 href= 属性值区间
// 注意：传入 cache 时，同一份 html 的 searchPos 必须单调不减
bool FindNextHtmlUriAttribute(
    std::string_view html,
    size_t searchPos,
    size_t& outUriStart,
    size_t& outUriEnd,
    char& outQuoteChar,
    size_t& outNextSearchPos,
    HtmlUriScanCache* cache = nullptr) noexcept {
    HtmlUriScanCache localCache;
    HtmlUriScanCache& c = cache ? *cache : localCache;
    while (searchPos < html.size()) {
        if (!c.srcDone && (!c.initialized || (c.srcPos != std::string_view::npos && c.srcPos < searchPos))) {
            c.srcPos = FindIgnoreCase(html, "src", searchPos);
            if (c.srcPos == std::string_view::npos) c.srcDone = true;
        }
        if (!c.hrefDone && (!c.initialized || (c.hrefPos != std::string_view::npos && c.hrefPos < searchPos))) {
            c.hrefPos = FindIgnoreCase(html, "href", searchPos);
            if (c.hrefPos == std::string_view::npos) c.hrefDone = true;
        }
        c.initialized = true;
        const size_t srcPos  = c.srcDone  ? std::string_view::npos : c.srcPos;
        const size_t hrefPos = c.hrefDone ? std::string_view::npos : c.hrefPos;
        size_t attrPos = std::string_view::npos;
        size_t attrNameLen = 0;

        if (srcPos != std::string_view::npos && (hrefPos == std::string_view::npos || srcPos <= hrefPos)) {
            attrPos = srcPos;
            attrNameLen = 3;
        } else if (hrefPos != std::string_view::npos) {
            attrPos = hrefPos;
            attrNameLen = 4;
        } else {
            return false;
        }

        // 检查前导字符边界，避免匹配到其他单词尾部
        if (attrPos > 0) {
            const char prev = html[attrPos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n' &&
                prev != '"' && prev != '\'' && prev != ':' && prev != '<') {
                searchPos = attrPos + attrNameLen;
                continue;
            }
        }

        // 跳过属性名与 '=' 之间的空白符（含折行）
        size_t eqPos = attrPos + attrNameLen;
        while (eqPos < html.size() && (html[eqPos] == ' ' || html[eqPos] == '\t' || html[eqPos] == '\r' || html[eqPos] == '\n')) {
            ++eqPos;
        }
        if (eqPos >= html.size() || html[eqPos] != '=') {
            searchPos = attrPos + attrNameLen;
            continue;
        }

        // 跳过 '=' 与属性值之间的所有空白符（修复 Word 80列折行导致的 href=\r\n"..." 漏匹配！）
        size_t valStart = eqPos + 1;
        while (valStart < html.size() && (html[valStart] == ' ' || html[valStart] == '\t' || html[valStart] == '\r' || html[valStart] == '\n')) {
            ++valStart;
        }
        if (valStart >= html.size()) {
            return false;
        }

        const char quoteChar = html[valStart];
        size_t uriStart = valStart;
        size_t uriEnd = std::string_view::npos;

        if (quoteChar == '"' || quoteChar == '\'') {
            uriStart = valStart + 1;
            uriEnd = html.find(quoteChar, uriStart);
        } else {
            uriEnd = html.find_first_of(" \t\r\n>", uriStart);
        }

        if (uriEnd == std::string_view::npos || uriEnd <= uriStart || uriEnd - uriStart > 4096) {
            searchPos = valStart + 1;
            continue;
        }

        outUriStart = uriStart;
        outUriEnd = uriEnd;
        outQuoteChar = (quoteChar == '"' || quoteChar == '\'') ? quoteChar : '\0';
        outNextSearchPos = (outQuoteChar != '\0') ? (uriEnd + 1) : uriEnd;
        return true;
    }
    return false;
}

std::filesystem::path DetectHtmlBaseDir(std::string_view html, size_t* outBaseTagStart, size_t* outBaseTagEnd) {
    if (outBaseTagStart) *outBaseTagStart = std::string_view::npos;
    if (outBaseTagEnd)   *outBaseTagEnd   = std::string_view::npos;

    // 1. 优先检查 <head> 内的 <link rel=File-List href="..."> 或 <base href="...">
    //    因为 Word 的 SourceURL 指向的是用户的 .docx 文档目录，而 <link rel=File-List> 才真正指向 %TEMP%\msohtmlclip1\01！
    const std::string_view headScanView = html.substr(0, std::min<size_t>(html.size(), 65536ULL));
    const size_t baseTagPos = FindIgnoreCase(headScanView, "<base");
    if (baseTagPos != std::string_view::npos) {
        const size_t baseTagEnd = headScanView.find('>', baseTagPos);
        if (baseTagEnd != std::string_view::npos && baseTagEnd - baseTagPos < 1024) {
            if (outBaseTagStart) *outBaseTagStart = baseTagPos;
            if (outBaseTagEnd)   *outBaseTagEnd   = baseTagEnd + 1;

            const std::string_view baseTagView = headScanView.substr(baseTagPos, baseTagEnd - baseTagPos + 1);
            size_t uStart = 0, uEnd = 0, nextPos = 0;
            char q = '\0';
            if (FindNextHtmlUriAttribute(baseTagView, 0, uStart, uEnd, q, nextPos)) {
                std::wstring resolvedBase = ResolveHtmlLocalFilePath(baseTagView.substr(uStart, uEnd - uStart), {});
                if (!resolvedBase.empty() && resolvedBase.find(L':') != std::wstring::npos) {
                    DWORD attr = ::GetFileAttributesW(resolvedBase.c_str());
                    std::filesystem::path bp(resolvedBase);
                    std::filesystem::path candidate = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                        ? bp.lexically_normal()
                        : bp.parent_path().lexically_normal();
                    DWORD dirAttr = ::GetFileAttributesW(candidate.c_str());
                    if (dirAttr != INVALID_FILE_ATTRIBUTES && (dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                        return candidate;
                    }
                }
            }
        }
    }

    // 2. 从 <link rel=File-List href=".../clip_filelist.xml"> 提取真实的 Word 剪切板临时目录
    const size_t fileListPos = FindIgnoreCase(headScanView, "File-List");
    if (fileListPos != std::string_view::npos) {
        const size_t linkStart = headScanView.rfind('<', fileListPos);
        const size_t linkEnd   = headScanView.find('>', fileListPos);
        if (linkStart != std::string_view::npos && linkEnd != std::string_view::npos && linkEnd > linkStart) {
            const std::string_view linkTag = headScanView.substr(linkStart, linkEnd - linkStart + 1);
            size_t uStart = 0, uEnd = 0, nextPos = 0;
            char q = '\0';
            if (FindNextHtmlUriAttribute(linkTag, 0, uStart, uEnd, q, nextPos)) {
                std::wstring resolvedList = ResolveHtmlLocalFilePath(linkTag.substr(uStart, uEnd - uStart), {});
                if (!resolvedList.empty() && resolvedList.find(L':') != std::wstring::npos) {
                    std::filesystem::path dir = std::filesystem::path(resolvedList).parent_path().lexically_normal();
                    DWORD dirAttr = ::GetFileAttributesW(dir.c_str());
                    if (dirAttr != INVALID_FILE_ATTRIBUTES && (dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                        return dir;
                    }
                }
            }
        }
    }

    // 3. 探测系统 %TEMP% 下的 Word 剪切板临时目录（置于 SourceURL 之前！防止被 .docx 所在目录截胡）
    //    注意：若 SourceURL 明确为 http(s) 网页且 HTML 未包含 Word/msohtmlclip 标记，则不误关联残留的 Word 临时目录。
    const std::string_view headerView = html.substr(0, std::min<size_t>(html.size(), 4096ULL));
    const size_t srcUrlPos = FindIgnoreCase(headerView, "SourceURL:");
    std::string_view rawSourceUrl;
    bool isWebSourceUrl = false;
    if (srcUrlPos != std::string_view::npos) {
        size_t valStart = srcUrlPos + 10;
        while (valStart < headerView.size() && (headerView[valStart] == ' ' || headerView[valStart] == '\t')) {
            ++valStart;
        }
        const size_t lineEnd = headerView.find_first_of("\r\n", valStart);
        rawSourceUrl = headerView.substr(
            valStart,
            (lineEnd == std::string_view::npos) ? (headerView.size() - valStart) : (lineEnd - valStart));
        isWebSourceUrl = StartsWithIgnoreCase(rawSourceUrl, "http://") ||
                         StartsWithIgnoreCase(rawSourceUrl, "https://");
    }

    if (!isWebSourceUrl ||
        FindIgnoreCase(headScanView, "msohtmlclip") != std::string_view::npos ||
        FindIgnoreCase(headScanView, "Word.Document") != std::string_view::npos) {
        wchar_t tempPath[MAX_PATH + 1]{};
        const DWORD tempLen = ::GetTempPathW(MAX_PATH, tempPath);
        if (tempLen > 0 && tempLen <= MAX_PATH) {
            const std::filesystem::path tempRoot(tempPath);
            const wchar_t* candidates[] = {
                L"msohtmlclip1\\01",
                L"msohtmlclip\\01",
                L"msohtmlclip1",
                L"msohtmlclip"
            };
            for (const wchar_t* sub : candidates) {
                std::filesystem::path candidate = tempRoot / sub;
                DWORD attr = ::GetFileAttributesW(candidate.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    return candidate.lexically_normal();
                }
            }
        }
    }

    // 4. 最后才兜底检查 CF_HTML 头部的 SourceURL:file:///...（针对非 Word 的本地 HTML 复制场景）
    if (!rawSourceUrl.empty() && !isWebSourceUrl) {
        std::wstring resolved = ResolveHtmlLocalFilePath(rawSourceUrl, {});
        if (!resolved.empty() && resolved.find(L':') != std::wstring::npos) {
            DWORD attr = ::GetFileAttributesW(resolved.c_str());
            std::filesystem::path p(resolved);
            std::filesystem::path dir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                ? p
                : p.parent_path();
            if (!dir.empty()) {
                DWORD dirAttr = ::GetFileAttributesW(dir.c_str());
                if (dirAttr != INVALID_FILE_ATTRIBUTES && (dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                    return dir.lexically_normal();
                }
            }
        }
    }

    return {};
}

std::string PathToFileUri(const std::wstring& widePath) {
    const std::string u8 = WideToUtf8(widePath);
    if (u8.empty()) {
        return {};
    }

    std::string uri = "file:///";
    uri.reserve(8 + u8.size() * 3);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : u8) {
        if (c == '\\') {
            uri.push_back('/');
        } else if (c == ' ' || c == '%' || c == '#' || c == '?' || c == '"' || c == '\'' || c < 0x20 || c >= 0x7F) {
            uri.push_back('%');
            uri.push_back(kHex[(c >> 4) & 0x0F]);
            uri.push_back(kHex[c & 0x0F]);
        } else {
            uri.push_back(static_cast<char>(c));
        }
    }
    return uri;
}

void PatchCfHtmlOffsets(std::string& html) {
    const std::string_view view(html);
    const size_t htmlTagPos = FindIgnoreCase(view, "<html");
    const size_t startFragMarker = FindIgnoreCase(view, "<!--StartFragment-->");
    const size_t endFragMarker = FindIgnoreCase(view, "<!--EndFragment-->");
    if (htmlTagPos == std::string_view::npos ||
        startFragMarker == std::string_view::npos ||
        endFragMarker == std::string_view::npos) {
        return;
    }

    if (startFragMarker < htmlTagPos || endFragMarker < startFragMarker + 20) return;

    // 重建固定宽度头，兼容原头的 -1、短数字、缺少 Selection 等情形。
    // 所有偏移都按 UTF-8 字节计算，而不是字符数；恢复发布完整 Fragment。
    std::string header = "Version:1.0\r\n";
    const char* keys[] = { "StartHTML:", "EndHTML:", "StartFragment:",
                          "EndFragment:", "StartSelection:", "EndSelection:" };
    for (const char* key : keys) {
        header.append(key);
        header.append("00000000000000000000\r\n");
    }
    const auto oldHeader = view.substr(0, htmlTagPos);
    const size_t sourcePos = FindIgnoreCase(oldHeader, "SourceURL:");
    if (sourcePos != std::string_view::npos) {
        const size_t sourceEnd = oldHeader.find_first_of("\r\n", sourcePos);
        header.append(oldHeader.substr(sourcePos, sourceEnd == std::string_view::npos
            ? oldHeader.size() - sourcePos : sourceEnd - sourcePos));
        header.append("\r\n");
    }
    const size_t startHtml = header.size();
    const size_t endHtml = startHtml + html.size() - htmlTagPos;
    const size_t startFragment = startHtml + startFragMarker + 20 - htmlTagPos;
    const size_t endFragment = startHtml + endFragMarker - htmlTagPos;
    const size_t offsets[] = { startHtml, endHtml, startFragment, endFragment,
                               startFragment, endFragment };
    for (size_t i = 0; i < 6; ++i) {
        const size_t pos = header.find(keys[i]) + std::strlen(keys[i]);
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%020zu", offsets[i]);
        std::memcpy(header.data() + pos, buf, 20);
    }
    header.append(view.substr(htmlTagPos));
    html.swap(header);
}

// 极速剥离 Word HTML 中庞大的 o:gfxdata="..." 与 o:hrdata="..." Base64 属性：
// 采用 memchr 块扫描，几十 MB 的数据可在数毫秒内完成瘦身。
void StripOfficeGfxDataInPlace(std::vector<uint8_t>& htmlBytes) {
    if (htmlBytes.size() < 64) return;
    const std::string_view view(reinterpret_cast<const char*>(htmlBytes.data()), htmlBytes.size());

    if (FindIgnoreCase(view, "o:gfxdata") == std::string_view::npos &&
        FindIgnoreCase(view, "o:hrdata") == std::string_view::npos) {
        return;
    }

    std::string cleaned;
    cleaned.reserve(htmlBytes.size() / 2);
    size_t lastPos = 0;
    size_t pos = 0;

    while (pos < view.size()) {
        const void* pColon = std::memchr(view.data() + pos, ':', view.size() - pos);
        if (!pColon) {
            break;
        }
        const size_t colonIdx = static_cast<size_t>(static_cast<const char*>(pColon) - view.data());
        if (colonIdx == 0) {
            pos = 1;
            continue;
        }

        const char prefixChar = view[colonIdx - 1];
        if (prefixChar != 'o' && prefixChar != 'O') {
            pos = colonIdx + 1;
            continue;
        }

        const size_t attrStart = colonIdx - 1;
        const std::string_view rem = view.substr(attrStart);
        size_t nameLen = 0;
        if (StartsWithIgnoreCase(rem, "o:gfxdata")) {
            nameLen = 9;
        } else if (StartsWithIgnoreCase(rem, "o:hrdata")) {
            nameLen = 8;
        } else {
            pos = colonIdx + 1;
            continue;
        }

        size_t eqPos = attrStart + nameLen;
        while (eqPos < view.size() && (view[eqPos] == ' ' || view[eqPos] == '\t' || view[eqPos] == '\r' || view[eqPos] == '\n')) {
            ++eqPos;
        }
        if (eqPos >= view.size() || view[eqPos] != '=') {
            pos = colonIdx + 1;
            continue;
        }

        size_t valStart = eqPos + 1;
        while (valStart < view.size() && (view[valStart] == ' ' || view[valStart] == '\t' || view[valStart] == '\r' || view[valStart] == '\n')) {
            ++valStart;
        }
        if (valStart < view.size() && (view[valStart] == '"' || view[valStart] == '\'')) {
            const char q = view[valStart];
            const size_t valEnd = view.find(q, valStart + 1);
            if (valEnd != std::string_view::npos) {
                size_t cutStart = attrStart;
                if (cutStart > lastPos && (view[cutStart - 1] == ' ' || view[cutStart - 1] == '\t')) {
                    --cutStart;
                }
                cleaned.append(view.data() + lastPos, cutStart - lastPos);
                lastPos = valEnd + 1;
                pos = lastPos;
                continue;
            }
        }
        pos = colonIdx + 1;
    }

    if (lastPos > 0) {
        cleaned.append(view.data() + lastPos, view.size() - lastPos);
        PatchCfHtmlOffsets(cleaned);
        htmlBytes.assign(cleaned.begin(), cleaned.end());
    }
}

void NormalizeWordHtmlForClipboardRestore(std::vector<uint8_t>& htmlBytes) {
    StripOfficeGfxDataInPlace(htmlBytes);
    if (htmlBytes.size() < 64) {
        return;
    }

    std::string html(reinterpret_cast<const char*>(htmlBytes.data()), htmlBytes.size());

    // 将 ProgId=Word.Document 改为 HTML.Document，强制 Word 走标准 HTML 导入器（无需 clip_filelist.xml 即可加载 <img>）
    {
        const size_t progIdPos = FindIgnoreCase(std::string_view(html).substr(0, std::min<size_t>(html.size(), 8192)), "Word.Document");
        if (progIdPos != std::string_view::npos) {
            html.replace(progIdPos, 13, "HTML.Document");
        }
    }

    const std::string_view view(html);
    if (FindIgnoreCase(view, "v:imagedata") == std::string_view::npos &&
        FindIgnoreCase(view, "!vml]") == std::string_view::npos) {
        PatchCfHtmlOffsets(html);
        htmlBytes.assign(html.begin(), html.end());
        return;
    }

    std::string out;
    out.reserve(html.size());

    size_t lastPos = 0;
    size_t pos = 0;
    while (pos < view.size()) {
        const void* pLt = std::memchr(view.data() + pos, '<', view.size() - pos);
        if (!pLt) {
            break;
        }
        pos = static_cast<size_t>(static_cast<const char*>(pLt) - view.data());
        const std::string_view rem = view.substr(pos);

        // 1. 处理 <!--[if gte vml 1]> ... <![endif]--> 块
        if (StartsWithIgnoreCase(rem, "<!--[if gte vml 1]>")) {
            const size_t endVml = FindIgnoreCase(view, "<![endif]-->", pos + 19);
            if (endVml != std::string_view::npos) {
                const size_t blockEnd = endVml + 12;
                const std::string_view vmlBlock = view.substr(pos, blockEnd - pos);

                // 检查紧随其后的 64 字节内是否存在 <![if !vml]> 兜底块
                size_t afterVml = blockEnd;
                while (afterVml < view.size() && (view[afterVml] == ' ' || view[afterVml] == '\t' || view[afterVml] == '\r' || view[afterVml] == '\n')) {
                    ++afterVml;
                }
                const std::string_view nextRem = view.substr(afterVml);
                const bool followedByNonVml = StartsWithIgnoreCase(nextRem, "<![if !vml]>") ||
                                              StartsWithIgnoreCase(nextRem, "<!--[if !vml]-->");

                out.append(view.data() + lastPos, pos - lastPos);

                if (!followedByNonVml) {
                    // 该 VML 块后面没有 <![if !vml]> 兜底！提取其中所有的 <v:imagedata src="..."/> 转为标准 <img>
                    size_t imgDataPos = 0;
                    while ((imgDataPos = FindIgnoreCase(vmlBlock, "<v:imagedata", imgDataPos)) != std::string_view::npos) {
                        const size_t tagEnd = vmlBlock.find('>', imgDataPos);
                        if (tagEnd == std::string_view::npos) break;
                        const std::string_view tagView = vmlBlock.substr(imgDataPos, tagEnd - imgDataPos + 1);
                        size_t uStart = 0, uEnd = 0, nextP = 0;
                        char q = '\0';
                        if (FindNextHtmlUriAttribute(tagView, 0, uStart, uEnd, q, nextP)) {
                            out.append("<img src=\"");
                            out.append(tagView.substr(uStart, uEnd - uStart));
                            out.append("\">");
                        }
                        imgDataPos = tagEnd + 1;
                    }
                }

                pos = blockEnd;
                lastPos = pos;
                continue;
            }
        }

        // 2. 剥离 <![if !vml]> / <!--[if !vml]--> / <![endif]> 外壳，暴露里面的 <img src="...">
        if (StartsWithIgnoreCase(rem, "<![if !vml]>")) {
            out.append(view.data() + lastPos, pos - lastPos);
            pos += 12;
            lastPos = pos;
            continue;
        }
        if (StartsWithIgnoreCase(rem, "<!--[if !vml]-->")) {
            out.append(view.data() + lastPos, pos - lastPos);
            pos += 16;
            lastPos = pos;
            continue;
        }
        if (StartsWithIgnoreCase(rem, "<![endif]>")) {
            out.append(view.data() + lastPos, pos - lastPos);
            pos += 10;
            lastPos = pos;
            continue;
        }

        // 3. 兜底处理未包裹在条件注释中的裸 <v:imagedata ...>
        if (StartsWithIgnoreCase(rem, "<v:imagedata")) {
            const size_t tagEnd = view.find('>', pos);
            if (tagEnd != std::string_view::npos) {
                const std::string_view tagView = view.substr(pos, tagEnd - pos + 1);
                size_t uStart = 0, uEnd = 0, nextP = 0;
                char q = '\0';
                if (FindNextHtmlUriAttribute(tagView, 0, uStart, uEnd, q, nextP)) {
                    out.append(view.data() + lastPos, pos - lastPos);
                    out.append("<img src=\"");
                    out.append(tagView.substr(uStart, uEnd - uStart));
                    out.append("\">");
                    pos = tagEnd + 1;
                    lastPos = pos;
                    continue;
                }
            }
        }

        ++pos;
    }

    out.append(view.data() + lastPos, view.size() - lastPos);
    PatchCfHtmlOffsets(out);
    htmlBytes.assign(out.begin(), out.end());
}

// 对 HTML 的 Fragment 正文进行“去噪全量哈希”（忽略 CF_HTML 头、<head>、折行差异及临时目录路径变化）
uint64_t ComputeCanonicalHtmlFragmentHash(std::string_view html, uint64_t seed) noexcept {
    // 1. 仅截取 <!--StartFragment--> 与 <!--EndFragment--> 之间的实质内容，彻底避开 <head> 中的 File-List 和 ProgId 噪音
    const size_t startFrag = FindIgnoreCase(html, "<!--StartFragment-->");
    const size_t endFrag   = FindIgnoreCase(html, "<!--EndFragment-->");
    std::string_view body  = html;
    if (startFrag != std::string_view::npos && endFrag != std::string_view::npos && endFrag > startFrag + 20) {
        body = html.substr(startFrag + 20, endFrag - (startFrag + 20));
    }

    uint64_t h = seed;
    auto mixByte = [&h](unsigned char b) noexcept {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ULL;
    };

    // 2. 扫描正文：跳过 \r\n 折行噪音，将 src/href 本地资源路径替换为固定序号（具体资源特征由 assetFeatureHash 保证）
    size_t pos = 0;
    size_t uriStart = 0, uriEnd = 0, nextSearchPos = 0;
    char quoteChar = '\0';
    HtmlUriScanCache cache;
    size_t resourceIdx = 0;

    while (FindNextHtmlUriAttribute(body, pos, uriStart, uriEnd, quoteChar, nextSearchPos, &cache)) {
        if (IsHtmlResourceUri(body, uriStart)) {
            // 哈希 URI 之前的正文内容（包含标签、style 样式、属性、正文文字）
            for (size_t i = pos; i < uriStart; ++i) {
                const char c = body[i];
                if (c != '\r' && c != '\n') {
                    mixByte(static_cast<unsigned char>(c));
                }
            }
            // 用稳定的资源序号替代每次可能变化的临时文件路径
            mixByte('#');
            mixByte(static_cast<unsigned char>(resourceIdx & 0xFF));
            ++resourceIdx;
            pos = uriEnd;
        } else {
            for (size_t i = pos; i < nextSearchPos; ++i) {
                const char c = body[i];
                if (c != '\r' && c != '\n') {
                    mixByte(static_cast<unsigned char>(c));
                }
            }
            pos = nextSearchPos;
        }
    }

    for (size_t i = pos; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\r' && c != '\n') {
            mixByte(static_cast<unsigned char>(c));
        }
    }
    return h;
}

// 对纯 RTF（无 HTML 时）计算去噪哈希，自动跳过 Word 每次随机生成的 \rsid / \insrsid / \shplid 等控制字
uint64_t ComputeCanonicalRtfHash(const std::vector<uint8_t>& rtfBytes, uint64_t seed) noexcept {
    uint64_t h = seed;
    const char* data = reinterpret_cast<const char*>(rtfBytes.data());
    const size_t size = rtfBytes.size();

    for (size_t i = 0; i < size;) {
        if (data[i] == '\\') {
            size_t cmdStart = i + 1;
            size_t cmdEnd = cmdStart;
            while (cmdEnd < size && ((data[cmdEnd] >= 'a' && data[cmdEnd] <= 'z') ||
                                     (data[cmdEnd] >= 'A' && data[cmdEnd] <= 'Z'))) {
                ++cmdEnd;
            }
            std::string_view cmd(data + cmdStart, cmdEnd - cmdStart);
            size_t numEnd = cmdEnd;
            if (numEnd < size && data[numEnd] == '-') ++numEnd;
            while (numEnd < size && data[numEnd] >= '0' && data[numEnd] <= '9') {
                ++numEnd;
            }
            // 忽略所有包含 rsid 的修订标记及 shplid 图形流水号
            if (FindIgnoreCase(cmd, "rsid") != std::string_view::npos ||
                StartsWithIgnoreCase(cmd, "shplid") ||
                StartsWithIgnoreCase(cmd, "blipuid")) {
                if (numEnd < size && data[numEnd] == ' ') ++numEnd;
                i = numEnd;
                continue;
            }
        }
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
        h *= 1099511628211ULL;
        ++i;
    }
    return h;
}

// 重型 HTML 的预览/纯文本回退来自已取得的 HTML，不再向 Word 请求第二种格式。
// 完整排版和对象仍以 htmlBytes 为准；这里不尝试实现 Word 文档模型。
std::wstring ExtractPlainTextFromCfHtml(const std::vector<uint8_t>& bytes) {
    const std::string_view html(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    size_t begin = FindIgnoreCase(html, "<!--StartFragment-->");
    begin = begin == std::string_view::npos ? 0 : begin + 20;
    size_t end = FindIgnoreCase(html, "<!--EndFragment-->", begin);
    if (end == std::string_view::npos) end = html.size();
    std::string text;
    for (size_t pos = begin; pos < end;) {
        if (html[pos] == '<') {
            if (html.substr(pos, 4) == "<!--") {
                const size_t close = html.find("-->", pos + 4);
                pos = close == std::string_view::npos ? end : close + 3;
                continue;
            }
            const size_t close = html.find('>', pos + 1);
            if (close == std::string_view::npos || close >= end) break;
            const auto tag = html.substr(pos + 1, close - pos - 1);
            if (StartsWithIgnoreCase(tag, "style") || StartsWithIgnoreCase(tag, "script")) {
                const auto key = StartsWithIgnoreCase(tag, "style") ? "</style" : "</script";
                const size_t closingTag = FindIgnoreCase(html, key, close + 1);
                const size_t closingEnd = closingTag == std::string_view::npos
                    ? std::string_view::npos : html.find('>', closingTag);
                pos = closingEnd == std::string_view::npos ? end : closingEnd + 1;
                continue;
            }
            if (StartsWithIgnoreCase(tag, "br") || StartsWithIgnoreCase(tag, "/p") ||
                StartsWithIgnoreCase(tag, "/div") || StartsWithIgnoreCase(tag, "/tr")) {
                text.push_back('\n');
            } else if (StartsWithIgnoreCase(tag, "/td")) {
                text.push_back('\t');
            }
            pos = close + 1;
            continue;
        }
        if (html[pos] == '&') {
            const size_t entityLength = std::min<size_t>(16, end - pos);
            const size_t relativeSemi = html.substr(pos, entityLength).find(';');
            const size_t semi = relativeSemi == std::string_view::npos
                ? std::string_view::npos : pos + relativeSemi;
            if (semi != std::string_view::npos && semi - pos <= 16) {
                const auto entity = html.substr(pos + 1, semi - pos - 1);
                uint32_t cp = 0;
                if (entity == "amp") cp = '&';
                else if (entity == "lt") cp = '<';
                else if (entity == "gt") cp = '>';
                else if (entity == "quot") cp = '"';
                else if (entity == "apos") cp = '\'';
                else if (entity == "nbsp") cp = 0xA0;
                else if (entity.starts_with("#")) {
                    size_t i = 1;
                    uint32_t radix = 10;
                    if (i < entity.size() && (entity[i] == 'x' || entity[i] == 'X')) {
                        radix = 16;
                        ++i;
                    }
                    for (; i < entity.size(); ++i) {
                        const int digit = HexVal(entity[i]);
                        if (digit < 0 || static_cast<uint32_t>(digit) >= radix ||
                            cp > (0x10FFFFU - static_cast<uint32_t>(digit)) / radix) {
                            cp = 0;
                            break;
                        }
                        cp = cp * radix + static_cast<uint32_t>(digit);
                    }
                }
                if (cp != 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                    if (cp < 0x80) text.push_back(static_cast<char>(cp));
                    else {
                        if (cp >= 0x10000) text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        if (cp >= 0x800) text.push_back(static_cast<char>(
                            (cp >= 0x10000 ? 0x80 : 0xE0) | ((cp >> 12) & 0x3F)));
                        text.push_back(static_cast<char>((cp >= 0x800 ? 0x80 : 0xC0) | ((cp >> 6) & 0x3F)));
                        text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    pos = semi + 1;
                    continue;
                }
            }
        }
        text.push_back(html[pos++]);
    }
    return Utf8OrAcpToWide(text);
}

// 判断文件名是否为 Word HTML 临时目录中的非图片辅助文件（不作为图片资产保存）
bool IsWordAuxiliaryHtmlFile(std::wstring_view lowerFileName) noexcept {
    return lowerFileName == L"clip.htm" ||
           lowerFileName == L"clip.html" ||
           lowerFileName == L"clip_filelist.xml" ||
           lowerFileName == L"filelist.xml" ||
           lowerFileName == L"clip_themedata.thmx" ||
           lowerFileName == L"themedata.thmx" ||
           lowerFileName == L"clip_colorschememapping.xml" ||
           lowerFileName == L"colorschememapping.xml" ||
           lowerFileName == L"clip_editdata.mso" ||
           lowerFileName == L"editdata.mso" ||
           lowerFileName == L"clip_header.htm";
}

namespace {

bool TryDecodeDataImageUri(std::string_view uriView, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (!StartsWithIgnoreCase(uriView, "data:image/")) {
        return false;
    }
    const size_t commaPos = uriView.find(',');
    if (commaPos == std::string_view::npos || commaPos > 128) {
        return false;
    }
    const std::string_view header = uriView.substr(0, commaPos);
    if (FindIgnoreCase(header, ";base64") == std::string_view::npos) {
        return false;
    }

    const std::string_view b64 = uriView.substr(commaPos + 1);
    if (b64.empty() || b64.size() > 24ULL * 1024ULL * 1024ULL) {
        return false;
    }

    auto b64Val = [](unsigned char c) noexcept -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    try {
        outBytes.reserve((b64.size() / 4) * 3);
        uint32_t buf = 0;
        int bits = 0;
        for (char ch : b64) {
            if (ch == '=') break;
            const int v = b64Val(static_cast<unsigned char>(ch));
            if (v < 0) continue;
            buf = (buf << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                outBytes.push_back(static_cast<uint8_t>((buf >> bits) & 0xFFU));
            }
        }
    } catch (...) {
        outBytes.clear();
        return false;
    }
    return !outBytes.empty();
}

} // namespace

bool ExtractFirstRtfPictBytes(const std::vector<uint8_t>& rtfBytes, std::vector<uint8_t>& outImageBytes) {
    outImageBytes.clear();
    if (rtfBytes.empty()) {
        return false;
    }

    const std::string_view rtf(reinterpret_cast<const char*>(rtfBytes.data()), rtfBytes.size());
    const size_t pngPos = FindIgnoreCase(rtf, "\\pngblip");
    const size_t jpgPos = FindIgnoreCase(rtf, "\\jpegblip");

    size_t blipPos = std::string_view::npos;
    size_t tokenLen = 0;
    if (pngPos != std::string_view::npos && (jpgPos == std::string_view::npos || pngPos <= jpgPos)) {
        blipPos = pngPos;
        tokenLen = 8; // "\pngblip"
    } else if (jpgPos != std::string_view::npos) {
        blipPos = jpgPos;
        tokenLen = 9; // "\jpegblip"
    } else {
        return false;
    }

    size_t pos = blipPos + tokenLen;
    // 跳过 \pngblip 之后紧跟的其他控制词（如 \picw...\pich...）或嵌套分组（如 {\*\blipuid ...}）
    int braceDepth = 0;
    while (pos < rtf.size()) {
        const char c = rtf[pos];
        if (c == '{') {
            ++braceDepth;
            ++pos;
        } else if (c == '}') {
            if (braceDepth > 0) {
                --braceDepth;
                ++pos;
            } else {
                return false;
            }
        } else if (braceDepth > 0) {
            ++pos;
        } else if (c == '\\') {
            ++pos;
            while (pos < rtf.size() &&
                   ((rtf[pos] >= 'a' && rtf[pos] <= 'z') ||
                    (rtf[pos] >= 'A' && rtf[pos] <= 'Z') ||
                    (rtf[pos] >= '0' && rtf[pos] <= '9') ||
                    rtf[pos] == '-')) {
                ++pos;
            }
            if (pos < rtf.size() && rtf[pos] == ' ') {
                ++pos;
            }
        } else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            ++pos;
        } else if (HexVal(c) >= 0) {
            break;
        } else {
            ++pos;
        }
    }

    const size_t closeBrace = rtf.find('}', pos);
    const size_t hexEnd = (closeBrace != std::string_view::npos) ? closeBrace : rtf.size();
    if (hexEnd <= pos) {
        return false;
    }

    const size_t maxHexSpan = std::min<size_t>(hexEnd - pos, 32ULL * 1024ULL * 1024ULL);
    try {
        outImageBytes.reserve(maxHexSpan / 2);
        int highNibble = -1;
        for (size_t i = pos; i < pos + maxHexSpan; ++i) {
            const int v = HexVal(rtf[i]);
            if (v < 0) continue;
            if (highNibble < 0) {
                highNibble = v;
            } else {
                outImageBytes.push_back(static_cast<uint8_t>((highNibble << 4) | v));
                highNibble = -1;
            }
        }
    } catch (...) {
        outImageBytes.clear();
        return false;
    }

    return !outImageBytes.empty();
}

void HtmlFormatProcessor::AnalyzeAndPrepareHtmlResources(
    std::vector<uint8_t>& htmlBytes,
    std::vector<std::wstring>& outAssetFiles,
    std::vector<uint8_t>* outFirstImageBytes,
    uint64_t* outAssetFeatureHash) {
    outAssetFiles.clear();
    if (outFirstImageBytes) {
        outFirstImageBytes->clear();
    }
    if (outAssetFeatureHash) {
        *outAssetFeatureHash = 0;
    }
    if (htmlBytes.empty()) {
        return;
    }

    // 1. 先在锁外剥离巨大的 o:gfxdata="..." 属性，将几十 MB 的 HTML 瞬间瘦身
    StripOfficeGfxDataInPlace(htmlBytes);

    const std::string_view html(reinterpret_cast<const char*>(htmlBytes.data()), htmlBytes.size());
    const std::filesystem::path baseDir = DetectHtmlBaseDir(html);

    std::unordered_map<std::wstring, uint64_t> seenFiles;
    uint64_t featureHash = kFnv1aOffsetBasis;

    auto recordLocalFileFast = [&](const std::wstring& localPath, uint64_t knownSize, bool sizeAlreadyKnown) {
        if (!IsLocalDrivePath(localPath)) {
            return;
        }
        std::wstring normKey = localPath;
        for (wchar_t& ch : normKey) {
            if (ch == L'/') ch = L'\\';
            ch = static_cast<wchar_t>(std::towlower(ch));
        }

        const size_t lastSlash = normKey.find_last_of(L'\\');
        const std::wstring_view fname = (lastSlash == std::wstring::npos)
            ? std::wstring_view(normKey)
            : std::wstring_view(normKey).substr(lastSlash + 1);

        if (IsWordAuxiliaryHtmlFile(fname) || !IsSupportedHtmlAssetExtension(fname)) {
            return;
        }

        if (seenFiles.contains(normKey)) {
            return;
        }

        uint64_t fsize = knownSize;
        if (!sizeAlreadyKnown) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(localPath.c_str(), GetFileExInfoStandard, &fad)) {
                // 仅当确实是 Office 剪切板临时目录中的图片尚未就绪/缺失时，才保留路径让持久化显式报错；
                // 普通网页/文档中夹带的无效本地死链直接忽略，避免整条复制记录保存失败。
                if (IsOfficeTempClipPath(normKey)) {
                    seenFiles.emplace(normKey, 0);
                    outAssetFiles.push_back(localPath);
                }
                return;
            }
            if ((fad.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                return;
            }
            fsize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | static_cast<uint64_t>(fad.nFileSizeLow);
        }

        if (fsize == 0 || fsize > kMaxHtmlAssetFileBytes) {
            return;
        }

        seenFiles.emplace(normKey, fsize);
        outAssetFiles.push_back(localPath);

        featureHash ^= fsize;
        featureHash *= 1099511628211ULL;

        // 读取图片文件：既用于富文本按需提取首张缩略图，也用于抽样文件头尾字节混入 featureHash（防止同尺寸不同图片误去重）
        std::ifstream ifs(localPath, std::ios::binary);
        if (ifs.is_open()) {
            const bool needFullFirstImg = outFirstImageBytes && outFirstImageBytes->empty() &&
                fsize <= 32ULL * 1024ULL * 1024ULL &&
                (fname.ends_with(L".png") || fname.ends_with(L".jpg") ||
                 fname.ends_with(L".jpeg") || fname.ends_with(L".gif") ||
                 fname.ends_with(L".bmp") || fname.ends_with(L".webp") ||
                 fname.ends_with(L".ico") || fname.ends_with(L".tif") ||
                 fname.ends_with(L".tiff") || fname.ends_with(L".dib") ||
                 fname.ends_with(L".emf") || fname.ends_with(L".wmf"));

            if (needFullFirstImg) {
                try {
                    std::vector<uint8_t> imgData(static_cast<size_t>(fsize));
                    if (ifs.read(reinterpret_cast<char*>(imgData.data()), static_cast<std::streamsize>(fsize))) {
                        constexpr size_t kSampleBytes = 4096ULL;
                        const size_t headLen = std::min<size_t>(imgData.size(), kSampleBytes);
                        featureHash = ComputeFnv1aHash(imgData.data(), headLen, featureHash);
                        if (imgData.size() > kSampleBytes) {
                            featureHash = ComputeFnv1aHash(
                                imgData.data() + (imgData.size() - kSampleBytes),
                                kSampleBytes,
                                featureHash);
                        }
                        *outFirstImageBytes = std::move(imgData);
                    }
                } catch (...) {
                }
            } else {
                // 仅快速抽样文件头部与尾部最多各 4KB 混入哈希，耗时极低且能精准区分不同图片内容
                char sampleBuf[4096];
                const auto headRead = static_cast<std::streamsize>(std::min<uint64_t>(fsize, sizeof(sampleBuf)));
                if (ifs.read(sampleBuf, headRead)) {
                    featureHash = ComputeFnv1aHash(sampleBuf, static_cast<size_t>(headRead), featureHash);
                }
                if (fsize > sizeof(sampleBuf)) {
                    ifs.seekg(static_cast<std::streamoff>(fsize - sizeof(sampleBuf)), std::ios::beg);
                    if (ifs.read(sampleBuf, sizeof(sampleBuf))) {
                        featureHash = ComputeFnv1aHash(sampleBuf, sizeof(sampleBuf), featureHash);
                    }
                }
            }
        }
    };

    // 只保存本次 HTML 实际引用的资源，不收集目录中的无关旧文件。
    size_t searchPos = 0;
    size_t uriStart = 0;
    size_t uriEnd = 0;
    char quoteChar = '\0';
    size_t nextSearchPos = 0;

    HtmlUriScanCache uriScanCache;
    while (FindNextHtmlUriAttribute(html, searchPos, uriStart, uriEnd, quoteChar, nextSearchPos, &uriScanCache)) {
        if (!IsHtmlResourceUri(html, uriStart)) {
            searchPos = nextSearchPos;
            continue;
        }
        const std::string_view uriView = html.substr(uriStart, uriEnd - uriStart);
        if (outFirstImageBytes && outFirstImageBytes->empty() &&
            StartsWithIgnoreCase(uriView, "data:image/")) {
            TryDecodeDataImageUri(uriView, *outFirstImageBytes);
        } else {
            const std::wstring localPath = ResolveHtmlLocalFilePath(uriView, baseDir);
            if (!localPath.empty()) {
                recordLocalFileFast(localPath, 0, false);
            }
        }
        searchPos = nextSearchPos;
    }

    if (outAssetFeatureHash && featureHash != kFnv1aOffsetBasis) {
        *outAssetFeatureHash = featureHash;
    }
}

bool HtmlFormatProcessor::PersistHtmlAssetsAndRewriteUris(
    std::vector<uint8_t>& htmlBytes,
    const std::vector<std::wstring>& sourceAssetFiles,
    const std::filesystem::path& targetAssetDir) {
    if (htmlBytes.empty() || sourceAssetFiles.empty()) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(targetAssetDir, ec);
    if (ec || !std::filesystem::exists(targetAssetDir, ec)) {
        return false;
    }

    // 必须是独立副本：源程序原地重写临时文件不能改变历史内容。
    std::unordered_map<std::wstring, std::string> oldPathToNewFileUri;
    std::unordered_map<std::wstring, std::string> fileNameToNewFileUri;

    for (const std::wstring& srcFileStr : sourceAssetFiles) {
        if (!IsLocalDrivePath(srcFileStr)) {
            continue;
        }

        std::filesystem::path srcPath(srcFileStr);
        std::wstring fileName = srcPath.filename().wstring();
        if (fileName.empty()) {
            continue;
        }

        std::wstring lowerName = fileName;
        for (wchar_t& ch : lowerName) ch = static_cast<wchar_t>(std::towlower(ch));
        if (IsWordAuxiliaryHtmlFile(lowerName) || !IsSupportedHtmlAssetExtension(lowerName)) {
            continue;
        }

        std::wstring normSrcKey = srcFileStr;
        for (wchar_t& ch : normSrcKey) {
            if (ch == L'/') ch = L'\\';
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        if (oldPathToNewFileUri.contains(normSrcKey)) {
            continue;
        }

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(srcFileStr.c_str(), GetFileExInfoStandard, &fad)) {
            if (IsOfficeTempClipPath(normSrcKey)) {
                return false;
            }
            continue;
        }
        if ((fad.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            continue;
        }
        const uint64_t fsize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | static_cast<uint64_t>(fad.nFileSizeLow);
        if (fsize == 0 || fsize > kMaxHtmlAssetFileBytes) {
            continue;
        }

        std::filesystem::path dstPath = targetAssetDir / fileName;
        size_t suffix = 0;
        while (::GetFileAttributesW(dstPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            dstPath = targetAssetDir / (srcPath.stem().wstring() + L"_" +
                std::to_wstring(++suffix) + srcPath.extension().wstring());
        }
        const std::wstring dstFileStr = dstPath.wstring();

        if (!::CopyFileW(srcFileStr.c_str(), dstFileStr.c_str(), TRUE)) return false;
        std::string newUri = PathToFileUri(dstPath.lexically_normal().wstring());
        oldPathToNewFileUri.emplace(std::move(normSrcKey), newUri);
        fileNameToNewFileUri.try_emplace(std::move(lowerName), std::move(newUri));
    }

    if (fileNameToNewFileUri.empty()) {
        std::filesystem::remove(targetAssetDir, ec);
        return false;
    }

    // 2. 先检测原 baseDir，再将 VML / <![if !vml]> 统一归一化为标准 <img src="...">
    size_t baseTagStart = std::string_view::npos;
    size_t baseTagEndPos = std::string_view::npos;
    const std::filesystem::path baseDir = DetectHtmlBaseDir(
        std::string_view(reinterpret_cast<const char*>(htmlBytes.data()), htmlBytes.size()),
        &baseTagStart,
        &baseTagEndPos);

    NormalizeWordHtmlForClipboardRestore(htmlBytes);

    // 归一化后重新定位 <base> 标签（若存在）
    const std::string_view html(reinterpret_cast<const char*>(htmlBytes.data()), htmlBytes.size());
    DetectHtmlBaseDir(html, &baseTagStart, &baseTagEndPos);

    // 3. 只重写实际资源引用，普通超链接与原始 base 保持不变。
    std::string outHtml;
    outHtml.reserve(html.size() + fileNameToNewFileUri.size() * 64 + 512);

    auto appendRange = [&](size_t from, size_t to) {
        if (from >= to) {
            return;
        }
        // 资源已重写为绝对 URI；保留原 base 以免改变普通相对超链接。
        outHtml.append(html.data() + from, to - from);
    };

    size_t lastCopiedPos = 0;
    size_t searchPos = 0;
    size_t uriStart = 0;
    size_t uriEnd = 0;
    char quoteChar = '\0';
    size_t nextSearchPos = 0;
    bool modified = false;

    HtmlUriScanCache rewriteScanCache;
    while (FindNextHtmlUriAttribute(html, searchPos, uriStart, uriEnd, quoteChar, nextSearchPos, &rewriteScanCache)) {
        const std::string_view uriView = html.substr(uriStart, uriEnd - uriStart);
        const std::wstring localPath = ResolveHtmlLocalFilePath(uriView, baseDir);

        const std::string* replacementUri = nullptr;
        std::wstring lowerFname;

        if (!localPath.empty()) {
            std::wstring normLookup = localPath;
            for (wchar_t& ch : normLookup) {
                if (ch == L'/') ch = L'\\';
                ch = static_cast<wchar_t>(std::towlower(ch));
            }
            const size_t slashPos = normLookup.find_last_of(L'\\');
            lowerFname = (slashPos == std::wstring::npos) ? normLookup : normLookup.substr(slashPos + 1);

            auto mapIt = oldPathToNewFileUri.find(normLookup);
            if (mapIt != oldPathToNewFileUri.end() && !mapIt->second.empty()) {
                replacementUri = &mapIt->second;
            }
        }

        // 移除失效的 <link rel=File-List / Edit-Time-Data / ThemeData> 标签
        if (IsWordAuxiliaryHtmlFile(lowerFname)) {
            const size_t tagStart = html.rfind('<', uriStart);
            const size_t tagEnd = html.find('>', uriEnd);
            if (tagStart != std::string_view::npos && tagStart >= lastCopiedPos &&
                tagEnd != std::string_view::npos && StartsWithIgnoreCase(html.substr(tagStart), "<link")) {
                appendRange(lastCopiedPos, tagStart);
                lastCopiedPos = tagEnd + 1;
                modified = true;
                searchPos = std::max(nextSearchPos, lastCopiedPos);
                continue;
            }
        }

        if (replacementUri && IsHtmlResourceUri(html, uriStart)) {
            appendRange(lastCopiedPos, uriStart);
            if (quoteChar == '\0') {
                outHtml.push_back('"');
                outHtml.append(*replacementUri);
                outHtml.push_back('"');
            } else {
                outHtml.append(*replacementUri);
            }
            lastCopiedPos = uriEnd;
            modified = true;
        }
        searchPos = nextSearchPos;
    }
    if (modified) {
        appendRange(lastCopiedPos, html.size());
        PatchCfHtmlOffsets(outHtml);
        htmlBytes.assign(outHtml.begin(), outHtml.end());
    }

    return true;
}

void HtmlFormatProcessor::RewriteStoredHtmlForClipboard(std::vector<uint8_t>& htmlBytes) {
    NormalizeWordHtmlForClipboardRestore(htmlBytes);
}

bool HtmlFormatProcessor::HtmlContentsEqual(
    const std::vector<uint8_t>& capturedHtml,
    const std::vector<uint8_t>& storedHtml,
    bool assetsWerePersisted) {
    if (capturedHtml.empty() || storedHtml.empty()) {
        return capturedHtml.empty() && storedHtml.empty();
    }

    // 使用与落盘相同的 Word 转换，避免拿原始 VML 与还原用的 <img> 比较。
    std::vector<uint8_t> normalizedCapture = capturedHtml;
    if (assetsWerePersisted) NormalizeWordHtmlForClipboardRestore(normalizedCapture);

    const auto canonicalize = [assetsWerePersisted](const std::vector<uint8_t>& bytes,
        std::string& result, std::vector<std::wstring>& resources) {
        const std::string_view html(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto baseDir = DetectHtmlBaseDir(html);
        size_t searchPos = 0;
        size_t lastCopiedPos = 0;
        size_t uriStart = 0;
        size_t uriEnd = 0;
        size_t nextSearchPos = 0;
        char quoteChar = '\0';
        HtmlUriScanCache cache;
        while (FindNextHtmlUriAttribute(html, searchPos, uriStart, uriEnd, quoteChar, nextSearchPos, &cache)) {
            const auto localPath = ResolveHtmlLocalFilePath(html.substr(uriStart, uriEnd - uriStart), baseDir);
            auto fileName = std::filesystem::path(localPath).filename().wstring();
            for (wchar_t& ch : fileName) ch = static_cast<wchar_t>(std::towlower(ch));
            // PersistHtmlAssetsAndRewriteUris 会移除这些辅助 link，不将其视为实际资源。
            if (assetsWerePersisted && IsWordAuxiliaryHtmlFile(fileName)) {
                const size_t tagStart = html.rfind('<', uriStart);
                const size_t tagEnd = html.find('>', uriEnd);
                if (tagStart != std::string_view::npos && tagStart >= lastCopiedPos &&
                    tagEnd != std::string_view::npos && StartsWithIgnoreCase(html.substr(tagStart), "<link")) {
                    result.append(html.substr(lastCopiedPos, tagStart - lastCopiedPos));
                    lastCopiedPos = tagEnd + 1;
                    searchPos = std::max(nextSearchPos, lastCopiedPos);
                    continue;
                }
            }
            if (!localPath.empty() && IsSupportedHtmlAssetExtension(fileName) && IsHtmlResourceUri(html, uriStart)) {
                result.append(html.substr(lastCopiedPos, uriStart - lastCopiedPos));
                // 无引号 URI 在落盘时会补双引号；已有引号保持不变。
                if (quoteChar == '\0') result.push_back('"');
                result.append("liteclip-resource-" + std::to_string(resources.size()));
                if (quoteChar == '\0') result.push_back('"');
                resources.push_back(localPath);
                lastCopiedPos = uriEnd;
            }
            searchPos = nextSearchPos;
        }
        result.append(html.substr(lastCopiedPos));
        // 资源 URI 长度变化会改变 CF_HTML 偏移，比较前重新计算而不是忽略内容。
        PatchCfHtmlOffsets(result);
    };

    std::string capturedCanonical;
    std::string storedCanonical;
    std::vector<std::wstring> capturedResources;
    std::vector<std::wstring> storedResources;
    canonicalize(normalizedCapture, capturedCanonical, capturedResources);
    canonicalize(storedHtml, storedCanonical, storedResources);
    if (capturedCanonical != storedCanonical || capturedResources.size() != storedResources.size()) return false;

    // 即便临时路径相同也必须读文件：源程序可能原地覆盖了同尺寸图片。
    for (size_t i = 0; i < capturedResources.size(); ++i) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(capturedResources[i], ec);
        if (ec) return false;
        const auto storedSize = std::filesystem::file_size(storedResources[i], ec);
        if (ec || size != storedSize) return false;
        std::ifstream capturedFile(std::filesystem::path(capturedResources[i]), std::ios::binary);
        std::ifstream storedFile(std::filesystem::path(storedResources[i]), std::ios::binary);
        if (!capturedFile.is_open() || !storedFile.is_open()) return false;
        char capturedBuffer[64 * 1024];
        char storedBuffer[64 * 1024];
        for (uintmax_t offset = 0; offset < size;) {
            const auto count = static_cast<std::streamsize>(std::min<uintmax_t>(sizeof(capturedBuffer), size - offset));
            capturedFile.read(capturedBuffer, count);
            storedFile.read(storedBuffer, count);
            if (capturedFile.gcount() != count || storedFile.gcount() != count ||
                std::memcmp(capturedBuffer, storedBuffer, static_cast<size_t>(count)) != 0) return false;
            offset += static_cast<uintmax_t>(count);
        }
    }
    return true;
}

