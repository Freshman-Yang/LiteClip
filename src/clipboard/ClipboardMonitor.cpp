#include "clipboard/ClipboardMonitor.h"
#include "clipboard/HtmlFormatProcessor.h"
#include "clipboard/OfficePolicy.h"
#include "core/Hash.h"
#include "platform/GlobalMemory.h"

#include <cstring>
#include <cwchar>
#include <string_view>

ClipboardMonitor::ClipboardMonitor() = default;

ClipboardMonitor::~ClipboardMonitor() {
    Stop();
}

bool ClipboardMonitor::Start(HWND hwnd, bool registerListener) {
    if (!hwnd || !::IsWindow(hwnd)) {
        return false;
    }

    if (listening_) {
        if (hwnd_ == hwnd) {
            return true;
        }
        Stop();
    }

    if (!codec_.EnsureWicFactory()) {
        return false;
    }

    cfHtml_ = ::RegisterClipboardFormatW(L"HTML Format");
    cfRtf_ = ::RegisterClipboardFormatW(L"Rich Text Format");
    cfPng_ = ::RegisterClipboardFormatW(L"PNG");
    cfObjectDescriptor_ = ::RegisterClipboardFormatW(L"Object Descriptor");
    cfObjectLink_ = ::RegisterClipboardFormatW(L"ObjectLink");
    cfExcludeHistory_ = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    cfCanIncludeHistory_ = ::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    cfViewerIgnore_ = ::RegisterClipboardFormatW(L"ClipboardViewerIgnore");

    if (cfHtml_ == 0 || cfRtf_ == 0 || cfPng_ == 0) {
        return false;
    }

    if (registerListener && !::AddClipboardFormatListener(hwnd)) {
        return false;
    }

    hwnd_ = hwnd;
    listening_ = registerListener;
    return true;
}

void ClipboardMonitor::Stop() {
    if (listening_ && hwnd_ && ::IsWindow(hwnd_)) {
        ::RemoveClipboardFormatListener(hwnd_);
    }
    listening_ = false;
    hwnd_ = nullptr;
    codec_.Reset();
}

bool ClipboardMonitor::OpenClipboardWithRetry(HWND hwnd) const {
    for (int i = 0; i < 5; ++i) {
        if (::OpenClipboard(hwnd)) {
            return true;
        }
        if (i < 4) {
            ::Sleep(10);
        }
    }
    return false;
}

bool ClipboardMonitor::IsExcludedByPrivacyFormats() const {
    if (cfExcludeHistory_ != 0 && ::IsClipboardFormatAvailable(cfExcludeHistory_)) {
        return true;
    }

    if (cfViewerIgnore_ != 0 && ::IsClipboardFormatAvailable(cfViewerIgnore_)) {
        return true;
    }

    if (cfCanIncludeHistory_ != 0 && ::IsClipboardFormatAvailable(cfCanIncludeHistory_)) {
        HANDLE hData = ::GetClipboardData(cfCanIncludeHistory_);
        if (hData && ::GlobalSize(hData) >= sizeof(DWORD)) {
            ScopedGlobalLock lock(hData);
            if (lock && *static_cast<const DWORD*>(lock.Get()) == 0) {
                return true;
            }
        }
    }

    return false;
}

bool ClipboardMonitor::IsStillInSystemClipboard(DWORD capturedSeqNum) noexcept {
    if (capturedSeqNum == 0) {
        return false;
    }
    const DWORD currentSeq = ::GetClipboardSequenceNumber();
    return currentSeq != 0 && currentSeq == capturedSeqNum;
}

bool ClipboardMonitor::OnClipboardUpdate(RawClipboardPayload& outPayload, bool* outTransientRetry) {
    if (outTransientRetry) {
        *outTransientRetry = false;
    }

    if (isInternalWrite_) {
        return false;
    }

    const DWORD preOpenSeq = ::GetClipboardSequenceNumber();
    if (lastSelfSeqNum_ != 0 && preOpenSeq == lastSelfSeqNum_) {
        return false;
    }

    outPayload = RawClipboardPayload{};

    if (!OpenClipboardWithRetry(hwnd_)) {
        if (outTransientRetry) {
            *outTransientRetry = true;
        }
        return false;
    }
    ScopedClipboard clipGuard(true);

    const DWORD lockedSeqNum = ::GetClipboardSequenceNumber();
    if (lastSelfSeqNum_ != 0 && lockedSeqNum == lastSelfSeqNum_) {
        return false;
    }

    if (IsExcludedByPrivacyFormats()) {
        return false;
    }

    const bool hasRtf  = (cfRtf_ != 0 && ::IsClipboardFormatAvailable(cfRtf_));
    const bool hasHtml = (cfHtml_ != 0 && ::IsClipboardFormatAvailable(cfHtml_));
    const bool hasUnicodeText = ::IsClipboardFormatAvailable(CF_UNICODETEXT);
    const bool hasImage =
        (cfPng_ != 0 && ::IsClipboardFormatAvailable(cfPng_)) ||
        ::IsClipboardFormatAvailable(CF_DIBV5) ||
        ::IsClipboardFormatAvailable(CF_DIB) ||
        ::IsClipboardFormatAvailable(CF_BITMAP);

    if (!hasRtf && !hasHtml && !hasUnicodeText && !hasImage) {
        // 若剪贴板没有任何格式（如外部软件刚调用 EmptyClipboard 准备分阶段写入），标记需要短暂重试
        if (::CountClipboardFormats() == 0 && outTransientRetry) {
            *outTransientRetry = true;
        }
        return false;
    }

    bool imageBeforeText = false;
    for (UINT fmt = ::EnumClipboardFormats(0); fmt != 0; fmt = ::EnumClipboardFormats(fmt)) {
        if (fmt == CF_UNICODETEXT || (cfHtml_ != 0 && fmt == cfHtml_)) {
            imageBeforeText = false;
            break;
        }
        if ((cfPng_ != 0 && fmt == cfPng_) || fmt == CF_DIBV5 || fmt == CF_DIB || fmt == CF_BITMAP) {
            imageBeforeText = true;
            break;
        }
    }

    const bool preferText = hasUnicodeText ? (!hasImage || !imageBeforeText || hasHtml || hasRtf)
                                           : (hasHtml || hasRtf);

    // ==========================================
    // 分支 1：优先处理文本/富文本
    // ==========================================
    if (preferText) {
        if (CopyTextAndRichTextFromClipboard(outPayload)) {
            outPayload.clipboardSeqNum = ::GetClipboardSequenceNumber();

            bool isTextless = IsAllWhitespace(outPayload.plainText);
            if (isTextless) {
                outPayload.plainText.clear();
            }
            // 若无文本或文本不足以填满 1 倍高度卡片，才按需提取首张图片；长文本直接跳过首图读取与解码以保证零性能损耗
            bool needPreviewImage = isTextless || !DoesTextFillSingleCard(outPayload.plainText);

            ClipboardImageSnapshot fallbackImageSnapshot;
            const bool hasFallbackSnapshot = isTextless && !hasHtml && outPayload.htmlBytes.empty() && hasImage &&
                                             CopyImageSnapshotFromClipboard(fallbackImageSnapshot);

            // 1. 锁内内存快照复制完毕，立即释放剪贴板锁！
            clipGuard.CloseNow();

            std::vector<uint8_t> firstHtmlImageBytes;
            uint64_t assetFeatureHash = 0;
            if (!outPayload.htmlBytes.empty()) {
                // 2. 在锁外剥离体积庞大的 o:gfxdata、内嵌 Base64 等冗余资源
                HtmlFormatProcessor::AnalyzeAndPrepareHtmlResources(
                    outPayload.htmlBytes,
                    outPayload.htmlLocalAssetFiles,
                    needPreviewImage ? &firstHtmlImageBytes : nullptr,
                    &assetFeatureHash);

                // 3. 在 HTML 瘦身之后提取纯文本，大幅减少 DOM/正则解析开销
                if (isTextless) {
                    outPayload.plainText = ExtractPlainTextFromCfHtml(outPayload.htmlBytes);
                    isTextless = IsAllWhitespace(outPayload.plainText);
                    if (isTextless) {
                        outPayload.plainText.clear();
                    }
                    needPreviewImage = isTextless || !DoesTextFillSingleCard(outPayload.plainText);
                }
            }

            if (needPreviewImage && firstHtmlImageBytes.empty() && !outPayload.rtfBytes.empty()) {
                ExtractFirstRtfPictBytes(outPayload.rtfBytes, firstHtmlImageBytes);
            }

            FinalizeTextPayload(outPayload, assetFeatureHash);

            // 4. 若无文本或文本未填满 1 倍高度，解码首张缩略图：
            //    - 无文本时：fallback 转为纯图片类型 (ClipItemType::Image)
            //    - 短文本时：保留 ClipItemType::RichText 与富文本哈希，仅挂载 pngBytes 供 UI 左文右图预览
            if (needPreviewImage) {
                bool extractedThumb = false;
                if (!firstHtmlImageBytes.empty()) {
                    extractedThumb = codec_.TryDecodeImageBytesToPng(
                        firstHtmlImageBytes,
                        outPayload,
                        kRichTextThumbnailMaxDim);
                }
                if (!extractedThumb && isTextless && hasFallbackSnapshot) {
                    RawClipboardPayload imgPayload{};
                    if (ProcessImageSnapshotToPng(fallbackImageSnapshot, imgPayload)) {
                        outPayload.pngBytes = std::move(imgPayload.pngBytes);
                        outPayload.imageWidth = imgPayload.imageWidth;
                        outPayload.imageHeight = imgPayload.imageHeight;
                        extractedThumb = true;
                    }
                }
                if (extractedThumb && isTextless) {
                    outPayload.plainText.clear();
                    outPayload.type = ClipItemType::Image;
                    uint64_t imgHash = ComputeFnv1aHash(
                        outPayload.pngBytes.data(),
                        outPayload.pngBytes.size(),
                        kFnv1aOffsetBasis);
                    if (assetFeatureHash != 0) {
                        imgHash ^= assetFeatureHash;
                        imgHash *= 1099511628211ULL;
                    }
                    outPayload.contentHash = imgHash;
                }
            }
            return true;
        }

        if (hasImage) {
            ClipboardImageSnapshot imageSnapshot;
            if (CopyImageSnapshotFromClipboard(imageSnapshot)) {
                const DWORD postImgSeq = ::GetClipboardSequenceNumber();
                clipGuard.CloseNow();
                if (ProcessImageSnapshotToPng(imageSnapshot, outPayload)) {
                    outPayload.clipboardSeqNum = postImgSeq;
                    return true;
                }
            }
        }
        return false;
    }

    // ==========================================
    // 分支 2：纯图片通道 / 降级回退通道
    // ==========================================
    ClipboardImageSnapshot imageSnapshot;
    const bool hasImageSnapshot = hasImage && CopyImageSnapshotFromClipboard(imageSnapshot);

    RawClipboardPayload fallbackTextPayload;
    const bool hasFallbackText = !hasImageSnapshot && (hasUnicodeText || hasHtml || hasRtf) &&
                                 CopyTextAndRichTextFromClipboard(fallbackTextPayload);

    const DWORD finalSeqNum = ::GetClipboardSequenceNumber();
    clipGuard.CloseNow();

    if (hasImageSnapshot && ProcessImageSnapshotToPng(imageSnapshot, outPayload)) {
        outPayload.clipboardSeqNum = finalSeqNum;
        return true;
    }

    if (hasFallbackText) {
        outPayload = std::move(fallbackTextPayload);
        outPayload.clipboardSeqNum = finalSeqNum;

        bool isTextless = IsAllWhitespace(outPayload.plainText);
        if (isTextless) {
            outPayload.plainText.clear();
        }
        bool needPreviewImage = isTextless || !DoesTextFillSingleCard(outPayload.plainText);

        std::vector<uint8_t> firstHtmlImageBytes;
        uint64_t assetFeatureHash = 0;
        if (!outPayload.htmlBytes.empty()) {
            HtmlFormatProcessor::AnalyzeAndPrepareHtmlResources(
                outPayload.htmlBytes,
                outPayload.htmlLocalAssetFiles,
                needPreviewImage ? &firstHtmlImageBytes : nullptr,
                &assetFeatureHash);

            // 同步调整：在 HTML 瘦身后补充提取纯文本
            if (isTextless) {
                outPayload.plainText = ExtractPlainTextFromCfHtml(outPayload.htmlBytes);
                isTextless = IsAllWhitespace(outPayload.plainText);
                if (isTextless) {
                    outPayload.plainText.clear();
                }
                needPreviewImage = isTextless || !DoesTextFillSingleCard(outPayload.plainText);
            }
        }

        if (needPreviewImage && firstHtmlImageBytes.empty() && !outPayload.rtfBytes.empty()) {
            ExtractFirstRtfPictBytes(outPayload.rtfBytes, firstHtmlImageBytes);
        }

        FinalizeTextPayload(outPayload, assetFeatureHash);

        if (needPreviewImage && !firstHtmlImageBytes.empty() &&
            codec_.TryDecodeImageBytesToPng(firstHtmlImageBytes, outPayload, kRichTextThumbnailMaxDim)) {
            if (isTextless) {
                outPayload.plainText.clear();
                outPayload.type = ClipItemType::Image;
                uint64_t imgHash = ComputeFnv1aHash(
                    outPayload.pngBytes.data(),
                    outPayload.pngBytes.size(),
                    kFnv1aOffsetBasis);
                if (assetFeatureHash != 0) {
                    imgHash ^= assetFeatureHash;
                    imgHash *= 1099511628211ULL;
                }
                outPayload.contentHash = imgHash;
            }
        }
        return true;
    }

    return false;
}

bool ClipboardMonitor::CopyImageSnapshotFromClipboard(ClipboardImageSnapshot& outSnapshot) const {
    outSnapshot = ClipboardImageSnapshot{};

    if (cfPng_ != 0 && ::IsClipboardFormatAvailable(cfPng_)) {
        if (TryCopyPngFromClipboard(outSnapshot.bytes)) {
            outSnapshot.kind = ImageSnapshotKind::PngBytes;
            return true;
        }
    }

    if (::IsClipboardFormatAvailable(CF_DIBV5)) {
        if (TryCopyDibFromClipboard(CF_DIBV5, outSnapshot.bytes)) {
            outSnapshot.kind = ImageSnapshotKind::DibBytes;
            return true;
        }
    }

    if (::IsClipboardFormatAvailable(CF_DIB)) {
        if (TryCopyDibFromClipboard(CF_DIB, outSnapshot.bytes)) {
            outSnapshot.kind = ImageSnapshotKind::DibBytes;
            return true;
        }
    }

    if (::IsClipboardFormatAvailable(CF_BITMAP)) {
        if (TryCopyHBitmapFromClipboard(outSnapshot.bytes, outSnapshot.width, outSnapshot.height)) {
            outSnapshot.kind = ImageSnapshotKind::BgraPixels;
            return true;
        }
    }

    return false;
}

bool ClipboardMonitor::TryCopyPngFromClipboard(std::vector<uint8_t>& outPngBytes) const {
    outPngBytes.clear();

    HANDLE hData = ::GetClipboardData(cfPng_);
    if (!hData) {
        return false;
    }

    const SIZE_T dataSize = ::GlobalSize(hData);
    if (dataSize < 33) {
        return false;
    }

    ScopedGlobalLock lock(hData);
    if (!lock) {
        return false;
    }

    const auto* rawBytes = static_cast<const uint8_t*>(lock.Get());

    static constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(rawBytes, kPngSignature, 8) != 0) {
        return false;
    }

    if (ReadBigEndianU32(rawBytes + 8) != 13U || std::memcmp(rawBytes + 12, "IHDR", 4) != 0) {
        return false;
    }
    const uint32_t ihdrWidth = ReadBigEndianU32(rawBytes + 16);
    const uint32_t ihdrHeight = ReadBigEndianU32(rawBytes + 20);
    if (ihdrWidth == 0 || ihdrHeight == 0 || ihdrWidth > kMaxImageDim || ihdrHeight > kMaxImageDim) {
        return false;
    }

    size_t actualPngSize = dataSize;
    size_t pos = 8;
    while (pos + 12 <= dataSize) {
        const uint32_t chunkLen = ReadBigEndianU32(rawBytes + pos);
        const size_t nextPos = pos + 12ULL + static_cast<size_t>(chunkLen);
        if (nextPos > dataSize) {
            break;
        }
        if (std::memcmp(rawBytes + pos + 4, "IEND", 4) == 0) {
            actualPngSize = nextPos;
            break;
        }
        pos = nextPos;
    }

    try {
        outPngBytes.assign(rawBytes, rawBytes + actualPngSize);
    } catch (...) {
        return false;
    }
    return true;
}

bool ClipboardMonitor::TryCopyDibFromClipboard(UINT format, std::vector<uint8_t>& outDibBytes) const {
    outDibBytes.clear();

    HANDLE hDib = ::GetClipboardData(format);
    if (!hDib) {
        return false;
    }

    const SIZE_T totalSize = ::GlobalSize(hDib);
    if (totalSize < sizeof(BITMAPINFOHEADER)) {
        return false;
    }

    ScopedGlobalLock lock(hDib);
    if (!lock) {
        return false;
    }

    const auto* basePtr = static_cast<const uint8_t*>(lock.Get());

    uint32_t width = 0;
    uint32_t height = 0;
    bool isTopDown = false;
    uint16_t bitCount = 0;
    DWORD compression = 0;
    size_t pixelOffset = 0;
    size_t srcStride = 0;
    size_t requiredPixelBytes = 0;

    if (!ValidateDibHeaderAndRange(
            basePtr,
            totalSize,
            width,
            height,
            isTopDown,
            bitCount,
            compression,
            pixelOffset,
            srcStride,
            requiredPixelBytes)) {
        return false;
    }

    const size_t copyBytes = pixelOffset + requiredPixelBytes;
    try {
        outDibBytes.assign(basePtr, basePtr + copyBytes);
    } catch (...) {
        return false;
    }
    return true;
}

bool ClipboardMonitor::TryCopyHBitmapFromClipboard(
    std::vector<uint8_t>& outBgraPixels,
    uint32_t& outW,
    uint32_t& outH) const {
    outBgraPixels.clear();
    outW = 0;
    outH = 0;

    auto hBitmap = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP));
    if (!hBitmap) {
        return false;
    }

    BITMAP bm = {};
    if (::GetObjectW(hBitmap, sizeof(BITMAP), &bm) != sizeof(BITMAP)) {
        return false;
    }

    if (bm.bmWidth <= 0 || bm.bmHeight == 0) {
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(bm.bmWidth);
    const uint32_t height = static_cast<uint32_t>(bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight);
    if (width == 0 || height == 0 || width > kMaxImageDim || height > kMaxImageDim) {
        return false;
    }

    const size_t stride = static_cast<size_t>(width) * 4ULL;
    const size_t totalBytes = stride * static_cast<size_t>(height);
    if (!FitsWicBuffer(totalBytes)) {
        return false;
    }

    try {
        outBgraPixels.resize(totalBytes);
    } catch (...) {
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = ::GetDC(nullptr);
    if (!hdcScreen) {
        outBgraPixels.clear();
        return false;
    }

    const int copiedLines = ::GetDIBits(
        hdcScreen,
        hBitmap,
        0,
        height,
        outBgraPixels.data(),
        &bmi,
        DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, hdcScreen);

    if (copiedLines != static_cast<int>(height)) {
        outBgraPixels.clear();
        return false;
    }

    for (size_t i = 3; i < outBgraPixels.size(); i += 4) {
        outBgraPixels[i] = 0xFFU;
    }

    outW = width;
    outH = height;
    return true;
}

bool ClipboardMonitor::ProcessImageSnapshotToPng(
    ClipboardImageSnapshot& snapshot,
    RawClipboardPayload& outPayload) {
    if (snapshot.kind == ImageSnapshotKind::None || snapshot.bytes.empty()) {
        return false;
    }

    if (!codec_.EnsureWicFactory()) {
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    bool extracted = false;

    switch (snapshot.kind) {
    case ImageSnapshotKind::PngBytes:
        extracted = codec_.TryExtractPngFromBytes(snapshot.bytes, outPayload, width, height);
        break;
    case ImageSnapshotKind::DibBytes:
        extracted = codec_.TryExtractDibFromBytes(snapshot.bytes, outPayload, width, height);
        break;
    case ImageSnapshotKind::BgraPixels:
        extracted = codec_.TryExtractBgraPixelsToPng(
            snapshot.bytes,
            snapshot.width,
            snapshot.height,
            outPayload,
            width,
            height);
        break;
    default:
        break;
    }

    if (!extracted || outPayload.pngBytes.empty() || width == 0 || height == 0) {
        outPayload.pngBytes.clear();
        return false;
    }

    outPayload.type = ClipItemType::Image;
    outPayload.imageWidth = width;
    outPayload.imageHeight = height;
    outPayload.contentHash = ComputeFnv1aHash(
        outPayload.pngBytes.data(),
        outPayload.pngBytes.size(),
        kFnv1aOffsetBasis);

    return true;
}

bool ClipboardMonitor::ReadRawFormatBytes(UINT format, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (format == 0 || !::IsClipboardFormatAvailable(format)) {
        return false;
    }

    HANDLE hData = ::GetClipboardData(format);
    if (!hData) {
        return false;
    }

    const SIZE_T rawSize = ::GlobalSize(hData);
    if (rawSize == 0) {
        return false;
    }

    ScopedGlobalLock lock(hData);
    if (!lock) {
        return false;
    }

    const auto* bytePtr = static_cast<const uint8_t*>(lock.Get());

    // HGLOBAL 的分配大小不等于字符串长度，必须从头定位首个终止符。
    size_t validLen = rawSize;
    if (const void* firstNull = std::memchr(bytePtr, 0, rawSize)) {
        validLen = static_cast<size_t>(static_cast<const uint8_t*>(firstNull) - bytePtr);
    }

    while (validLen > 0 && bytePtr[validLen - 1] == 0) {
        --validLen;
    }

    if (validLen == 0) {
        return false;
    }

    try {
        outBytes.assign(bytePtr, bytePtr + validLen);
    } catch (...) {
        outBytes.clear();
        return false;
    }
    return true;
}

bool ClipboardMonitor::CopyTextAndRichTextFromClipboard(RawClipboardPayload& outPayload) const {
    outPayload.plainText.clear();
    outPayload.htmlBytes.clear();
    outPayload.rtfBytes.clear();
    outPayload.htmlLocalAssetFiles.clear();

    // =========================================================================
    // 【第 1 步：读取 CF_UNICODETEXT 并统计总段落数 (totalLines) 与空行数 (emptyLines)】
    // =========================================================================
    size_t textByteSize = 0;
    PlainTextMetrics metrics{};

    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hText = ::GetClipboardData(CF_UNICODETEXT);
        if (hText) {
            const SIZE_T rawByteSize = ::GlobalSize(hText);
            if (rawByteSize >= sizeof(wchar_t)) {
                ScopedGlobalLock lock(hText);
                if (lock) {
                    const auto* wchPtr = static_cast<const wchar_t*>(lock.Get());
                    const size_t maxChars = rawByteSize / sizeof(wchar_t);

                    size_t charLen = maxChars;
                    if (const wchar_t* nullPos = std::wmemchr(wchPtr, L'\0', maxChars)) {
                        charLen = static_cast<size_t>(nullPos - wchPtr);
                    }
                    if (charLen > 0 && wchPtr[charLen - 1] >= 0xD800 && wchPtr[charLen - 1] <= 0xDBFF) {
                        --charLen;
                    }
                    if (charLen > 0) {
                        try {
                            outPayload.plainText.assign(wchPtr, charLen);
                        } catch (...) {
                            return false;
                        }
                        textByteSize = charLen * sizeof(wchar_t);
                        metrics = AnalyzePlainTextMetrics(outPayload.plainText);
                    }
                }
            }
        }
    }

    // 绝对兜底熔断：无论来自什么软件（哪怕是浏览器或代码编辑器），纯文本超 512KB 或行数超 5000 行直接只存纯文本
    if (textByteSize > TierLimits::kGreenLargePlainTextBytes ||
        metrics.totalLines > TierLimits::kGreenLargeTotalLines) {
        return !IsAllWhitespace(outPayload.plainText);
    }

    // =========================================================================
    // 【第 2 步：零开销嗅探源文档大小，执行 Word 三级预判分流】
    // =========================================================================
    const bool hasWordOleHints =
        (cfObjectDescriptor_ != 0 && ::IsClipboardFormatAvailable(cfObjectDescriptor_)) ||
        (cfObjectLink_ != 0 && ::IsClipboardFormatAvailable(cfObjectLink_));

    bool allowTryRtf = true;

    if (hasWordOleHints) {
        const uint64_t docSize = SniffWordDocumentSizeFromClipboard(cfObjectDescriptor_, cfObjectLink_);
        const bool isGreenZone = (docSize > 0 && docSize <= TierLimits::kSafeDocFileBytes);

        if (isGreenZone) {
            // 🟢 绿灯区（源文件 <= 20 MB）：不会卡死，放心读 HTML；仅在小范围时读 RTF
            allowTryRtf = (metrics.emptyLines <= TierLimits::kGreenSmallMaxEmptyLines &&
                           textByteSize <= TierLimits::kGreenSmallMaxPlainTextBytes);
        } else {
            // 🔴 警戒区（源文件 > 20 MB 如 400MB 测试文档，或 docSize == 0 未保存文档）：
            // 【3. 大型内容（果断熔断）】：空行 > 18 或 总段落 > 90 或 文本 > 80KB
            // 绝不触碰 HTML 和 RTF！0 毫秒耗时直接返回纯文本，彻底消灭 50 章卡死 10 秒的问题！
            if (metrics.emptyLines > TierLimits::kAlertMediumMaxEmptyLines ||
                metrics.totalLines > TierLimits::kAlertMediumMaxTotalLines ||
                textByteSize > TierLimits::kAlertMediumMaxPlainTextBytes) {
                return !IsAllWhitespace(outPayload.plainText);
            }

            // 【1. 小型内容 vs 2. 中型内容】：
            // 空行 <= 8（约 1~7 章）、总段落 <= 40、文本 <= 30KB 才允许尝试 RTF；
            // 落在 8~16 章区间则 allowTryRtf = false，只走 HTML + 独立资源副本。
            allowTryRtf = (metrics.emptyLines <= TierLimits::kAlertSmallMaxEmptyLines &&
                           metrics.totalLines <= TierLimits::kAlertSmallMaxTotalLines &&
                           textByteSize <= TierLimits::kAlertSmallMaxPlainTextBytes);
        }
    } else {
        // 非 Word / 普通富文本（如网页、记事本、IDE）：按文本大小决定是否读 RTF
        allowTryRtf = (textByteSize <= TierLimits::kGreenSmallMaxPlainTextBytes);
    }

    // =========================================================================
    // 【第 3 步：读取 HTML Format 并统计渲染耗时】
    // =========================================================================
    const bool offersHtml = (cfHtml_ != 0 && ::IsClipboardFormatAvailable(cfHtml_));
    bool hasHtml = false;
    uint64_t htmlFetchElapsedMs = 0;

    if (offersHtml) {
        const uint64_t t0 = ::GetTickCount64();
        hasHtml = ReadRawFormatBytes(cfHtml_, outPayload.htmlBytes);
        htmlFetchElapsedMs = ::GetTickCount64() - t0;
    }

    // =========================================================================
    // 【第 4 步：事后校验（防漏网之鱼）—— 决定最终是否触碰 RTF】
    // 若单行内塞了大量图片导致导出 HTML 耗时超过 800ms，或 HTML 临时目录图片超标，立即中止碰 RTF！
    // =========================================================================
    bool allowReadRtf = false;
    if (allowTryRtf && htmlFetchElapsedMs <= TierLimits::kMaxHtmlFetchMsForRtf) {
        if (hasHtml) {
            allowReadRtf = EvaluateHtmlForSmallTier(outPayload.htmlBytes);
        } else if (!offersHtml) {
            allowReadRtf = true;
        }
    }

    bool hasRtf = false;
    if (allowReadRtf && cfRtf_ != 0 && ::IsClipboardFormatAvailable(cfRtf_)) {
        hasRtf = ReadRawFormatBytes(cfRtf_, outPayload.rtfBytes);
    }

    if (!hasHtml && !hasRtf && IsAllWhitespace(outPayload.plainText)) {
        outPayload.plainText.clear();
        return false;
    }

    return !outPayload.plainText.empty() || hasHtml || hasRtf;
}

void ClipboardMonitor::FinalizeTextPayload(RawClipboardPayload& outPayload, uint64_t assetFeatureHash) {
    if (!outPayload.htmlBytes.empty() || !outPayload.rtfBytes.empty()) {
        outPayload.type = ClipItemType::RichText;
    } else {
        outPayload.type = ClipItemType::PlainText;
    }

    uint64_t hash = ComputeFnv1aHash(
        outPayload.plainText.data(),
        outPayload.plainText.size() * sizeof(wchar_t),
        kFnv1aOffsetBasis);

    if (!outPayload.htmlBytes.empty()) {
        // 先克隆一份或直接对已剥离 o:gfxdata 的 HTML 进行 VML 归一化后再算哈希，确保捕获态与持久化态一致
        std::vector<uint8_t> normHtml = outPayload.htmlBytes;
        NormalizeWordHtmlForClipboardRestore(normHtml);
        const std::string_view htmlView(
            reinterpret_cast<const char*>(normHtml.data()),
            normHtml.size());
        hash = ComputeCanonicalHtmlFragmentHash(htmlView, hash);
        if (assetFeatureHash != 0) {
            hash ^= assetFeatureHash;
            hash *= 1099511628211ULL;
        }
    } else if (!outPayload.rtfBytes.empty()) {
        // 当剪贴板只有 RTF 而无 HTML（如写字板 WordPad）时，使用剔除动态 RSID 的 RTF 哈希
        hash = ComputeCanonicalRtfHash(outPayload.rtfBytes, hash);
    }

    outPayload.contentHash = hash;
}

bool ClipboardMonitor::WriteToClipboard(HWND hwnd, const RawClipboardPayload& payload, bool plainTextOnly, HWND expectedTarget) {
    // 第一条记录序列号直通粘贴：若系统剪贴板未被改动，零写入直接返回 true
    if (!plainTextOnly && IsStillInSystemClipboard(payload.clipboardSeqNum)) {
        return true;
    }

    std::vector<uint8_t> restoredHtml;
    if (!plainTextOnly && !payload.htmlBytes.empty()) {
        restoredHtml = payload.htmlBytes;
        HtmlFormatProcessor::RewriteStoredHtmlForClipboard(restoredHtml);
    }

    ScopedGlobalMem hPng;
    ScopedGlobalMem hDib;
    ScopedGlobalMem hUnicode;
    ScopedGlobalMem hHtml;
    ScopedGlobalMem hRtf;

    if (payload.type == ClipItemType::Image) {
        const bool hasRichSource = !plainTextOnly && (!payload.htmlBytes.empty() || !payload.rtfBytes.empty());

        if (hasRichSource) {
            if (cfRtf_ == 0) {
                cfRtf_ = ::RegisterClipboardFormatW(L"Rich Text Format");
            }
            if (cfHtml_ == 0) {
                cfHtml_ = ::RegisterClipboardFormatW(L"HTML Format");
            }
            if (cfRtf_ != 0 && !payload.rtfBytes.empty()) {
                hRtf = ScopedGlobalMem(CreateGlobalBuffer(payload.rtfBytes.data(), payload.rtfBytes.size(), true));
            }
            if (cfHtml_ != 0 && !payload.htmlBytes.empty()) {
                hHtml = ScopedGlobalMem(CreateGlobalBuffer(restoredHtml.data(), restoredHtml.size(), true));
            }
        }

        // 仅当该图片项不是来自富文本（或者富文本句柄创建失败）时，才写入单张缩略图的 PNG/DIB，
        // 防止 Word 粘贴第 2 条以后的无字多图/高清图记录时，优先读取了 800px 的缩略图位图而忽略 CF_HTML！
        if (!hHtml && !hRtf) {
            if (payload.pngBytes.empty()) {
                return false;
            }

            if (cfPng_ == 0) {
                cfPng_ = ::RegisterClipboardFormatW(L"PNG");
            }
            if (cfPng_ != 0) {
                hPng = ScopedGlobalMem(CreateGlobalBuffer(payload.pngBytes.data(), payload.pngBytes.size(), false));
            }

            HGLOBAL rawDib = nullptr;
            if (codec_.DecodePngToDib(payload.pngBytes, rawDib)) {
                hDib = ScopedGlobalMem(rawDib);
            }
        }

        if (!hPng && !hDib && !hHtml && !hRtf) {
            return false;
        }
    } else {
        if (payload.plainText.empty() && payload.rtfBytes.empty() && payload.htmlBytes.empty()) {
            return false;
        }

        const size_t textByteLen = (payload.plainText.size() + 1ULL) * sizeof(wchar_t);
        hUnicode = ScopedGlobalMem(CreateGlobalBuffer(payload.plainText.c_str(), textByteLen, false));
        if (!hUnicode) {
            return false;
        }

        if (!plainTextOnly && payload.type == ClipItemType::RichText) {
            if (cfRtf_ == 0) {
                cfRtf_ = ::RegisterClipboardFormatW(L"Rich Text Format");
            }
            if (cfHtml_ == 0) {
                cfHtml_ = ::RegisterClipboardFormatW(L"HTML Format");
            }

            if (cfRtf_ != 0 && !payload.rtfBytes.empty()) {
                hRtf = ScopedGlobalMem(CreateGlobalBuffer(payload.rtfBytes.data(), payload.rtfBytes.size(), true));
            }
            if (cfHtml_ != 0 && !payload.htmlBytes.empty()) {
                hHtml = ScopedGlobalMem(CreateGlobalBuffer(restoredHtml.data(), restoredHtml.size(), true));
            }
        }
    }

    if (cfExcludeHistory_ == 0) {
        cfExcludeHistory_ = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    }
    ScopedGlobalMem hExclude;
    if (cfExcludeHistory_ != 0) {
        const DWORD excludeFlag = 1U;
        hExclude = ScopedGlobalMem(CreateGlobalBuffer(&excludeFlag, sizeof(excludeFlag), false));
    }

    HWND targetHwnd = hwnd ? hwnd : hwnd_;

    isInternalWrite_ = true;
    ScopedBoolReset resetGuard(isInternalWrite_);

    if (!OpenClipboardWithRetry(targetHwnd)) {
        return false;
    }
    ScopedClipboard clipGuard(true);

    if (expectedTarget && ::GetForegroundWindow() != expectedTarget) return false;
    if (!::EmptyClipboard()) {
        return false;
    }

    bool wroteAny = false;

    if (payload.type == ClipItemType::Image) {
        if (!plainTextOnly) {
            if (SetClipboardGlobalData(cfRtf_, hRtf)) {
                wroteAny = true;
            }
            if (SetClipboardGlobalData(cfHtml_, hHtml)) {
                wroteAny = true;
            }
        }
        if (SetClipboardGlobalData(cfPng_, hPng)) {
            wroteAny = true;
        }
        if (SetClipboardGlobalData(CF_DIB, hDib)) {
            wroteAny = true;
        }
    } else {
        if (!plainTextOnly && payload.type == ClipItemType::RichText) {
            if (SetClipboardGlobalData(cfRtf_, hRtf)) {
                wroteAny = true;
            }
            if (SetClipboardGlobalData(cfHtml_, hHtml)) {
                wroteAny = true;
            }
        }

        if (SetClipboardGlobalData(CF_UNICODETEXT, hUnicode)) {
            wroteAny = true;
        }
    }

    if (wroteAny) {
        SetClipboardGlobalData(cfExcludeHistory_, hExclude);
    }

    clipGuard.CloseNow();

    lastSelfSeqNum_ = ::GetClipboardSequenceNumber();

    return wroteAny;
}

