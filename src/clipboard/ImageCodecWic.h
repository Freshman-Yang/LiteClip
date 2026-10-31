#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include <wincodec.h>
#include <cstddef>
#include <cstdint>
#include <vector>

inline constexpr uint32_t kMaxImageDim               = 32768U;
inline constexpr DWORD    kBiAlphaBitfields          = 6U; // BI_ALPHABITFIELDS
// 无字富文本提取首张图片作为 UI 卡片缩略图时的最大长边像素（卡片高度仅 160 DIP，800px 绰绰有余）
inline constexpr uint32_t kRichTextThumbnailMaxDim   = 800U;

uint32_t ReadBigEndianU32(const uint8_t* ptr) noexcept;

bool ValidateDibHeaderAndRange(
    const uint8_t* basePtr,
    size_t totalSize,
    uint32_t& outWidth,
    uint32_t& outHeight,
    bool& outIsTopDown,
    uint16_t& outBitCount,
    DWORD& outCompression,
    size_t& outPixelOffset,
    size_t& outSrcStride,
    size_t& outRequiredPixelBytes) noexcept;

class ImageCodecWic {
public:
    ImageCodecWic() = default;
    ~ImageCodecWic() = default;

    bool EnsureWicFactory();
    void Reset() noexcept;

    bool EncodeBitmapSourceToPng(
        IWICBitmapSource* source,
        std::vector<uint8_t>& outPng,
        uint32_t& outW,
        uint32_t& outH);

    bool TryExtractPngFromBytes(
        std::vector<uint8_t>& pngBytes,
        RawClipboardPayload& outPayload,
        uint32_t& outW,
        uint32_t& outH);

    bool TryExtractDibFromBytes(
        const std::vector<uint8_t>& dibBytes,
        RawClipboardPayload& outPayload,
        uint32_t& outW,
        uint32_t& outH);

    bool TryExtractBgraPixelsToPng(
        const std::vector<uint8_t>& bgraPixels,
        uint32_t width,
        uint32_t height,
        RawClipboardPayload& outPayload,
        uint32_t& outW,
        uint32_t& outH);

    bool DecodePngToDib(
        const std::vector<uint8_t>& pngBytes,
        HGLOBAL& outDibGlobal);

    bool TryDecodeImageBytesToPng(
        const std::vector<uint8_t>& rawImageBytes,
        RawClipboardPayload& outPayload,
        uint32_t maxDimension = 0);

private:
    ComPtr<IWICImagingFactory> wicFactory_;
};
