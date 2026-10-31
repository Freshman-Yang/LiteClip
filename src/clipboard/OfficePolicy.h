#pragma once

#include "platform/Win32Common.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// ============================================================================
// Word / 富文本三级动态分级阈值配置（小 / 中 / 大）
// ============================================================================
namespace TierLimits {
    // 源文件安全体积分水岭：20 MB 以内的 .docx 文档不会出现数百 MB 解码卡死
    inline constexpr uint64_t kSafeDocFileBytes              = 20ULL * 1024ULL * 1024ULL;

    // ------------------------------------------------------------------------
    // 🟢 绿灯区（已保存且 0 < docSize <= 20 MB）：大胆放行
    // ------------------------------------------------------------------------
    inline constexpr size_t   kGreenLargePlainTextBytes      = 512ULL * 1024ULL; // 超过 512KB 纯文本才降级为纯文本
    inline constexpr size_t   kGreenLargeTotalLines          = 5000;
    inline constexpr size_t   kGreenSmallMaxEmptyLines       = 10;               // <= 10 个空行（约 10 张图）直通 RTF
    inline constexpr size_t   kGreenSmallMaxPlainTextBytes   = 50ULL * 1024ULL;  // <= 50 KB 纯文本直通 RTF

    // ------------------------------------------------------------------------
    // 🔴 警戒区（docSize > 20 MB 巨型文档，或 docSize == 0 未保存文档）
    // 针对测试文档校准：单章约 4 段、1 个图片空行、2KB 文本、8MB 噪点大图
    // ------------------------------------------------------------------------
    // 【1. 小型内容上限】（对应 1 ~ 7 章：保留 RTF + HTML + 纯文本，微卡可接受）
    inline constexpr size_t   kAlertSmallMaxEmptyLines       = 8;                // 空行数 <= 8（约 1~7 张大图）
    inline constexpr size_t   kAlertSmallMaxTotalLines       = 40;               // 总段落 <= 40 行
    inline constexpr size_t   kAlertSmallMaxPlainTextBytes   = 30ULL * 1024ULL;  // 纯文本 <= 30 KB

    // 【2. 中型内容上限 / 3. 大型内容熔断线】（对应 8 ~ 16 章走 HTML+资源副本；>=17 章直接秒切纯文本，绝不碰 HTML！）
    inline constexpr size_t   kAlertMediumMaxEmptyLines      = 18;               // 空行数 > 18 直接熔断为纯文本
    inline constexpr size_t   kAlertMediumMaxTotalLines      = 90;               // 总段落 > 90 直接熔断为纯文本
    inline constexpr size_t   kAlertMediumMaxPlainTextBytes  = 80ULL * 1024ULL;  // 纯文本 > 80 KB 直接熔断为纯文本

    // ------------------------------------------------------------------------
    // 【事后校验防线】：拿到 HTML 后决定是否继续读取 RTF（兼容 5~8 章高清噪点大图）
    // ------------------------------------------------------------------------
    inline constexpr uint64_t kMaxHtmlFetchMsForRtf          = 800;              // 若光是导出 HTML 就耗时 > 800ms，说明图片极重，严禁再碰 RTF！
    inline constexpr size_t   kSmallMaxHtmlBytes             = 2ULL * 1024ULL * 1024ULL; // 5~8 章带 VML/gfxdata 的 HTML 允许到 2 MB
    inline constexpr size_t   kSmallMaxImageCount            = 8;                // 允许最多 8 张图（对应 5~8 章测试文档）
    inline constexpr uint64_t kSmallMaxTotalImgBytes         = 80ULL * 1024ULL * 1024ULL; // 允许 8 张未压缩噪点大图（最高 80 MB）
}

struct PlainTextMetrics {
    size_t totalLines = 0; // 总行数/段落数
    size_t emptyLines = 0; // 空行数（包含连续回车及仅含空白符的段落，直接对应 Word 独立成行的图片数）
};

// 极速统计纯文本的总段落数与空行数（耗时 < 0.05ms）
PlainTextMetrics AnalyzePlainTextMetrics(std::wstring_view text) noexcept;

// 零延迟嗅探 Word 源文档大小（通过 Object Descriptor / ObjectLink 纯内存结构体，0ms 耗时，绝不触发 Word 渲染）
uint64_t SniffWordDocumentSizeFromClipboard(UINT cfObjectDescriptor, UINT cfObjectLink) noexcept;

inline uint64_t ProbeSourceDocFileSizeFast(UINT cfObjectDescriptor, UINT cfObjectLink) noexcept {
    return SniffWordDocumentSizeFromClipboard(cfObjectDescriptor, cfObjectLink);
}

// 【事后开箱评估】快速评估已读取的 HTML 是否足够轻量以允许安全读取 RTF（小型 vs 中型判定）
bool EvaluateHtmlForSmallTier(const std::vector<uint8_t>& htmlBytes);
