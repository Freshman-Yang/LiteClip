#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include "clipboard/ImageCodecWic.h"

class ClipboardMonitor {
public:
    ClipboardMonitor();
    ~ClipboardMonitor();

    bool Start(HWND hwnd, bool registerListener = true);
    void Stop();

    // 在收到 WM_CLIPBOARDUPDATE 时调用。
    // 若是本软件内部粘贴写入触发的变化，立即豁免并返回 false。
    // 若成功捕获到富文本、纯文本或图像，填充 outPayload 并返回 true。
    // 若因剪贴板被其他进程短暂锁定或外部软件正在分阶段写入导致暂态失败，*outTransientRetry 置为 true。
    bool OnClipboardUpdate(RawClipboardPayload& outPayload, bool* outTransientRetry = nullptr);

    // 判断指定序列号的条目是否仍完整保留在当前系统剪切板中（直通粘贴核心判据）
    static bool IsStillInSystemClipboard(DWORD capturedSeqNum) noexcept;

    // 将历史记录写回系统剪切板（用于单击粘贴或纯文本粘贴），自动开启内部操作豁免标记。
    // 【核心优化】：当 plainTextOnly == false 且 payload.clipboardSeqNum 与当前系统剪切板序列号一致时，
    // 自动触发“零写入直通模式”（直接返回 true，不破坏系统剪切板中 Word 原生的 OLE/RTF 延迟渲染数据）。
    bool WriteToClipboard(HWND hwnd, const RawClipboardPayload& payload, bool plainTextOnly, HWND expectedTarget = nullptr);

private:
    enum class ImageSnapshotKind : uint8_t {
        None = 0,
        PngBytes,
        DibBytes,
        BgraPixels
    };

    struct ClipboardImageSnapshot {
        ImageSnapshotKind kind = ImageSnapshotKind::None;
        std::vector<uint8_t> bytes;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    HWND hwnd_ = nullptr;
    bool listening_ = false;
    bool isInternalWrite_ = false;
    DWORD lastSelfSeqNum_ = 0;

    UINT cfHtml_ = 0;
    UINT cfRtf_ = 0;
    UINT cfPng_ = 0;
    UINT cfObjectDescriptor_ = 0;
    UINT cfObjectLink_ = 0;
    UINT cfExcludeHistory_ = 0;
    UINT cfCanIncludeHistory_ = 0;
    UINT cfViewerIgnore_ = 0;
    ImageCodecWic codec_;

    bool OpenClipboardWithRetry(HWND hwnd) const;
    bool IsExcludedByPrivacyFormats() const;

    // 锁内极速拷贝快照接口（仅内存读取，不做重度编解码与哈希计算）
    bool CopyImageSnapshotFromClipboard(ClipboardImageSnapshot& outSnapshot) const;
    bool TryCopyPngFromClipboard(std::vector<uint8_t>& outPngBytes) const;
    bool TryCopyDibFromClipboard(UINT format, std::vector<uint8_t>& outDibBytes) const;
    bool TryCopyHBitmapFromClipboard(std::vector<uint8_t>& outBgraPixels, uint32_t& outW, uint32_t& outH) const;
    bool CopyTextAndRichTextFromClipboard(RawClipboardPayload& outPayload) const;
    static bool ReadRawFormatBytes(UINT format, std::vector<uint8_t>& outBytes);

    // 锁外编解码与哈希计算接口
    bool ProcessImageSnapshotToPng(ClipboardImageSnapshot& snapshot, RawClipboardPayload& outPayload);
    static void FinalizeTextPayload(RawClipboardPayload& outPayload, uint64_t assetFeatureHash = 0);
};
