#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <filesystem>

#ifndef _MINWINDEF_
using DWORD = unsigned long;
using UINT  = unsigned int;
#endif

enum class ClipItemType : uint8_t {
    PlainText = 0,
    RichText  = 1,
    Image     = 2
};

enum class ActiveTab : uint8_t {
    History = 0,
    Pinned  = 1
};

// 内存常驻的轻量级索引结构（严禁常驻大块富文本或位图）
struct ClipItem {
    uint64_t id = 0;
    ClipItemType type = ClipItemType::PlainText;
    uint64_t contentHash = 0;
    int64_t timestamp = 0;
    DWORD clipboardSeqNum = 0;   // 捕获该条目时的系统剪切板序列号（用于第一条直通粘贴判断）
    bool isPinned = false;

    std::wstring previewText;    // 仅保留前 200 个字符用于 UI 卡片纯文本预览
    std::wstring textFileName;   // 完整 UTF-16 纯文本落盘文件名
    std::wstring htmlFileName;   // HTML Format 落盘文件名（可为空）
    std::wstring rtfFileName;    // RTF 落盘文件名（可为空）
    std::wstring imageFileName;  // 严格命名为 截图_YYYYMMDD_HHMMSS.png 的文件名
    std::wstring assetDirName;   // 富文本外部图片/资源专属持久化子目录名（如 assets_12，可为空）

    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
};

using ClipItems = std::deque<ClipItem>;

// 瞬态传输结构：仅在捕获瞬间、粘贴瞬间或拖拽瞬间按需加载
struct RawClipboardPayload {
    ClipItemType type = ClipItemType::PlainText;
    uint64_t contentHash = 0;
    DWORD clipboardSeqNum = 0;   // 捕获瞬间记录的系统 GetClipboardSequenceNumber()
    std::wstring plainText;
    std::vector<uint8_t> htmlBytes;
    std::vector<uint8_t> rtfBytes;
    std::vector<uint8_t> pngBytes;
    std::vector<std::wstring> htmlLocalAssetFiles; // HTML 中引用的本地临时文件绝对路径列表（供 StorageManager 转存）
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
};
