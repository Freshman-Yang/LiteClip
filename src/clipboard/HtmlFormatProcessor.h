#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// 跨文件复用的富文本/HTML 辅助函数
bool IsAllWhitespace(std::wstring_view str) noexcept;
// 零分配极速判定文本是否足以填满 1 倍高度卡片（3 行 / 约 288 DIP 宽），用于在捕获阶段决定是否跳过首图提取
bool DoesTextFillSingleCard(std::wstring_view text) noexcept;
// 从 RTF 字节流中按需提取首张内嵌图片（\pngblip / \jpegblip）原始字节
bool ExtractFirstRtfPictBytes(const std::vector<uint8_t>& rtfBytes, std::vector<uint8_t>& outImageBytes);
size_t FindIgnoreCase(std::string_view haystack, std::string_view needle, size_t startPos = 0) noexcept;
std::wstring Utf8OrAcpToWide(std::string_view str);
bool IsWordAuxiliaryHtmlFile(std::wstring_view lowerFileName) noexcept;

std::filesystem::path DetectHtmlBaseDir(
    std::string_view html,
    size_t* outBaseTagStart = nullptr,
    size_t* outBaseTagEnd = nullptr);
void NormalizeWordHtmlForClipboardRestore(std::vector<uint8_t>& htmlBytes);
std::wstring ExtractPlainTextFromCfHtml(const std::vector<uint8_t>& bytes);

// 去噪哈希计算
uint64_t ComputeCanonicalHtmlFragmentHash(std::string_view html, uint64_t seed) noexcept;
uint64_t ComputeCanonicalRtfHash(const std::vector<uint8_t>& rtfBytes, uint64_t seed) noexcept;

class HtmlFormatProcessor {
public:
    // 在锁外剥离 o:gfxdata 等冗余数据，解析并收集 HTML 实际引用的本地资源文件及特征哈希
    static void AnalyzeAndPrepareHtmlResources(
        std::vector<uint8_t>& htmlBytes,
        std::vector<std::wstring>& outAssetFiles,
        std::vector<uint8_t>* outFirstImageBytes = nullptr,
        uint64_t* outAssetFeatureHash = nullptr);

    // 供 StorageManager 在持久化落盘时调用：
    // 将 HTML 实际引用的本地资源独立拷贝到持久化目录 targetAssetDir，
    // 并将 htmlBytes 中的路径重写为指向 targetAssetDir 的标准 file:/// 绝对路径，同时修正 CF_HTML 偏移量。
    static bool PersistHtmlAssetsAndRewriteUris(
        std::vector<uint8_t>& htmlBytes,
        const std::vector<std::wstring>& sourceAssetFiles,
        const std::filesystem::path& targetAssetDir);

    // 供写回系统剪切板前调用：剥离 Office 冗余数据并将 Word VML 归一化为标准 HTML <img>
    static void RewriteStoredHtmlForClipboard(std::vector<uint8_t>& htmlBytes);

    // 比较捕获 HTML 与落盘 HTML；路径改写不算内容变化，本地资源必须逐字节相同。
    static bool HtmlContentsEqual(
        const std::vector<uint8_t>& capturedHtml,
        const std::vector<uint8_t>& storedHtml,
        bool assetsWerePersisted);
};

