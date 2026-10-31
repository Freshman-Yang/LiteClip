#include "clipboard/ImageCodecWic.h"
#include "core/Hash.h"
#include "platform/GlobalMemory.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

uint32_t ReadBigEndianU32(const uint8_t* ptr) noexcept {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8)  |
           static_cast<uint32_t>(ptr[3]);
}

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
    size_t& outRequiredPixelBytes) noexcept {
    if (!basePtr || totalSize < sizeof(BITMAPINFOHEADER)) {
        return false;
    }

    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(basePtr);
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > totalSize) {
        return false;
    }

    if (bih->biWidth <= 0 || bih->biHeight == 0) {
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(bih->biWidth);
    const bool isTopDown = (bih->biHeight < 0);
    const int64_t absHeight = isTopDown ? -static_cast<int64_t>(bih->biHeight) : static_cast<int64_t>(bih->biHeight);
    if (absHeight <= 0 || absHeight > static_cast<int64_t>(kMaxImageDim) || width > kMaxImageDim) {
        return false;
    }
    const uint32_t height = static_cast<uint32_t>(absHeight);
    if (!FitsWicBuffer(static_cast<size_t>(width) * height * 4ULL)) return false;

    const uint16_t bitCount = bih->biBitCount;
    const DWORD compression = bih->biCompression;

    if (bitCount != 24 && bitCount != 32) {
        return false;
    }
    if (compression != BI_RGB && compression != BI_BITFIELDS && compression != kBiAlphaBitfields) {
        return false;
    }

    size_t colorTableBytes = 0;
    if (bih->biSize == sizeof(BITMAPINFOHEADER)) {
        if (compression == BI_BITFIELDS) {
            colorTableBytes += 3 * sizeof(DWORD);
        } else if (compression == kBiAlphaBitfields) {
            colorTableBytes += 4 * sizeof(DWORD);
        }
    }
    if (bih->biClrUsed > 0) {
        if (bih->biClrUsed > 65536U) {
            return false;
        }
        colorTableBytes += static_cast<size_t>(bih->biClrUsed) * sizeof(RGBQUAD);
    }

    const size_t pixelOffset = static_cast<size_t>(bih->biSize) + colorTableBytes;
    if (pixelOffset >= totalSize) {
        return false;
    }

    const size_t srcStride = ((static_cast<size_t>(width) * bitCount + 31ULL) / 32ULL) * 4ULL;
    const size_t requiredPixelBytes = srcStride * static_cast<size_t>(height);
    if (pixelOffset + requiredPixelBytes > totalSize) {
        return false;
    }

    outWidth = width;
    outHeight = height;
    outIsTopDown = isTopDown;
    outBitCount = bitCount;
    outCompression = compression;
    outPixelOffset = pixelOffset;
    outSrcStride = srcStride;
    outRequiredPixelBytes = requiredPixelBytes;
    return true;
}

bool ImageCodecWic::EnsureWicFactory() {
    if (wicFactory_) {
        return true;
    }

    // COM 生命周期由调用线程统一管理。
    const HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory_));

    return SUCCEEDED(hr) && wicFactory_ != nullptr;
}

void ImageCodecWic::Reset() noexcept {
    wicFactory_.Reset();
}

bool ImageCodecWic::TryExtractPngFromBytes(
    std::vector<uint8_t>& pngBytes,
    RawClipboardPayload& outPayload,
    uint32_t& outW,
    uint32_t& outH) {
    if (pngBytes.size() < 8 || !FitsWicBuffer(pngBytes.size()) || !EnsureWicFactory()) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory_->CreateStream(&stream))) {
        return false;
    }

    if (FAILED(stream->InitializeFromMemory(pngBytes.data(), static_cast<DWORD>(pngBytes.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromStream(
            stream.Get(),
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    UINT w = 0;
    UINT h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > kMaxImageDim || h > kMaxImageDim) {
        return false;
    }

    outPayload.pngBytes = std::move(pngBytes);
    outW = static_cast<uint32_t>(w);
    outH = static_cast<uint32_t>(h);
    return true;
}

bool ImageCodecWic::TryExtractDibFromBytes(
    const std::vector<uint8_t>& dibBytes,
    RawClipboardPayload& outPayload,
    uint32_t& outW,
    uint32_t& outH) {
    if (!EnsureWicFactory()) {
        return false;
    }

    const uint8_t* basePtr = dibBytes.data();
    const size_t totalSize = dibBytes.size();

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

    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(basePtr);
    const uint8_t* srcPixels = basePtr + pixelOffset;

    if (bitCount == 32) {
        uint32_t rMask = 0x00FF0000U;
        uint32_t gMask = 0x0000FF00U;
        uint32_t bMask = 0x000000FFU;
        uint32_t aMask = 0xFF000000U;

        if (compression == BI_BITFIELDS || compression == kBiAlphaBitfields) {
            if (bih->biSize >= sizeof(BITMAPV4HEADER)) {
                const auto* v4 = reinterpret_cast<const BITMAPV4HEADER*>(bih);
                rMask = v4->bV4RedMask;
                gMask = v4->bV4GreenMask;
                bMask = v4->bV4BlueMask;
                aMask = v4->bV4AlphaMask;
            } else if (compression == kBiAlphaBitfields && bih->biSize + 4 * sizeof(DWORD) <= totalSize) {
                const auto* masks = reinterpret_cast<const DWORD*>(basePtr + bih->biSize);
                rMask = masks[0];
                gMask = masks[1];
                bMask = masks[2];
                aMask = masks[3];
            } else if (bih->biSize + 3 * sizeof(DWORD) <= totalSize) {
                const auto* masks = reinterpret_cast<const DWORD*>(basePtr + bih->biSize);
                rMask = masks[0];
                gMask = masks[1];
                bMask = masks[2];
                aMask = 0U;
            }
        }

        const size_t dstStride = static_cast<size_t>(width) * 4ULL;
        std::vector<uint8_t> topDownBgra;
        try {
            topDownBgra.resize(dstStride * static_cast<size_t>(height));
        } catch (...) {
            return false;
        }

        bool hasNonZeroAlpha = false;
        const bool isStandardBgra =
            (rMask == 0x00FF0000U) &&
            (gMask == 0x0000FF00U) &&
            (bMask == 0x000000FFU) &&
            (aMask == 0xFF000000U || aMask == 0U);

        auto extractChannel = [](uint32_t pixel, uint32_t mask) noexcept -> uint8_t {
            if (mask == 0) {
                return 0;
            }
            const int shift = std::countr_zero(mask);
            const uint32_t maxVal = mask >> shift;
            const uint32_t val = (pixel & mask) >> shift;
            if (maxVal == 0xFFU) {
                return static_cast<uint8_t>(val);
            }
            return static_cast<uint8_t>((static_cast<uint64_t>(val) * 255ULL + (maxVal / 2ULL)) / maxVal);
        };

        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t srcY = isTopDown ? y : (height - 1U - y);
            const uint8_t* srcRow = srcPixels + static_cast<size_t>(srcY) * srcStride;
            uint8_t* dstRow = topDownBgra.data() + static_cast<size_t>(y) * dstStride;

            if (isStandardBgra) {
                std::memcpy(dstRow, srcRow, dstStride);
                if (aMask != 0U && !hasNonZeroAlpha) {
                    for (uint32_t x = 0; x < width; ++x) {
                        if (dstRow[x * 4 + 3] != 0) {
                            hasNonZeroAlpha = true;
                            break;
                        }
                    }
                }
            } else {
                const auto* srcPx = reinterpret_cast<const uint32_t*>(srcRow);
                for (uint32_t x = 0; x < width; ++x) {
                    const uint32_t px = srcPx[x];
                    const uint8_t a = aMask ? extractChannel(px, aMask) : 0xFFU;
                    dstRow[x * 4 + 0] = extractChannel(px, bMask);
                    dstRow[x * 4 + 1] = extractChannel(px, gMask);
                    dstRow[x * 4 + 2] = extractChannel(px, rMask);
                    dstRow[x * 4 + 3] = a;
                    if (a != 0) {
                        hasNonZeroAlpha = true;
                    }
                }
            }
        }

        if (!hasNonZeroAlpha || ((compression == BI_BITFIELDS || compression == kBiAlphaBitfields) && aMask == 0U)) {
            for (size_t i = 3; i < topDownBgra.size(); i += 4) {
                topDownBgra[i] = 0xFFU;
            }
        }

        ComPtr<IWICBitmap> wicBitmap;
        HRESULT hr = wicFactory_->CreateBitmapFromMemory(
            width,
            height,
            GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(dstStride),
            static_cast<UINT>(topDownBgra.size()),
            topDownBgra.data(),
            &wicBitmap);
        if (FAILED(hr) || !wicBitmap) {
            return false;
        }

        return EncodeBitmapSourceToPng(wicBitmap.Get(), outPayload.pngBytes, outW, outH);
    }

    if (bitCount == 24) {
        std::vector<uint8_t> topDownBgr;
        try {
            topDownBgr.resize(requiredPixelBytes);
        } catch (...) {
            return false;
        }
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t srcY = isTopDown ? y : (height - 1U - y);
            const uint8_t* srcRow = srcPixels + static_cast<size_t>(srcY) * srcStride;
            uint8_t* dstRow = topDownBgr.data() + static_cast<size_t>(y) * srcStride;
            std::memcpy(dstRow, srcRow, srcStride);
        }

        ComPtr<IWICBitmap> wicBitmap;
        HRESULT hr = wicFactory_->CreateBitmapFromMemory(
            width,
            height,
            GUID_WICPixelFormat24bppBGR,
            static_cast<UINT>(srcStride),
            static_cast<UINT>(topDownBgr.size()),
            topDownBgr.data(),
            &wicBitmap);
        if (FAILED(hr) || !wicBitmap) {
            return false;
        }

        return EncodeBitmapSourceToPng(wicBitmap.Get(), outPayload.pngBytes, outW, outH);
    }

    return false;
}

bool ImageCodecWic::TryExtractBgraPixelsToPng(
    const std::vector<uint8_t>& bgraPixels,
    uint32_t width,
    uint32_t height,
    RawClipboardPayload& outPayload,
    uint32_t& outW,
    uint32_t& outH) {
    if (!FitsWicBuffer(bgraPixels.size()) || width == 0 || height == 0 || !EnsureWicFactory()) {
        return false;
    }

    const size_t stride = static_cast<size_t>(width) * 4ULL;
    if (bgraPixels.size() < stride * static_cast<size_t>(height)) {
        return false;
    }

    ComPtr<IWICBitmap> wicBitmap;
    HRESULT hr = wicFactory_->CreateBitmapFromMemory(
        width,
        height,
        GUID_WICPixelFormat32bppBGRA,
        static_cast<UINT>(stride),
        static_cast<UINT>(bgraPixels.size()),
        const_cast<BYTE*>(bgraPixels.data()),
        &wicBitmap);
    if (FAILED(hr) || !wicBitmap) {
        return false;
    }

    return EncodeBitmapSourceToPng(wicBitmap.Get(), outPayload.pngBytes, outW, outH);
}

bool ImageCodecWic::EncodeBitmapSourceToPng(
    IWICBitmapSource* source,
    std::vector<uint8_t>& outPng,
    uint32_t& outW,
    uint32_t& outH) {
    if (!source || !EnsureWicFactory()) {
        return false;
    }

    UINT w = 0;
    UINT h = 0;
    if (FAILED(source->GetSize(&w, &h)) || w == 0 || h == 0 || w > kMaxImageDim || h > kMaxImageDim) {
        return false;
    }

    ComPtr<IWICBitmapSource> encodeSource = source;
    WICPixelFormatGUID srcPixelFormat = GUID_WICPixelFormatDontCare;
    if (FAILED(source->GetPixelFormat(&srcPixelFormat)) ||
        !IsEqualGUID(srcPixelFormat, GUID_WICPixelFormat32bppBGRA)) {
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory_->CreateFormatConverter(&converter))) {
            return false;
        }
        if (FAILED(converter->Initialize(
                source,
                GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom))) {
            return false;
        }
        encodeSource = converter;
    }

    ComPtr<IStream> memStream;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &memStream))) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        return false;
    }

    if (FAILED(encoder->Initialize(memStream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> propBag;
    if (FAILED(encoder->CreateNewFrame(&frameEncode, &propBag))) {
        return false;
    }

    if (FAILED(frameEncode->Initialize(propBag.Get()))) {
        return false;
    }

    if (FAILED(frameEncode->SetSize(w, h))) {
        return false;
    }

    WICPixelFormatGUID targetFormat = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frameEncode->SetPixelFormat(&targetFormat))) {
        return false;
    }

    if (FAILED(frameEncode->WriteSource(encodeSource.Get(), nullptr))) {
        return false;
    }

    if (FAILED(frameEncode->Commit())) {
        return false;
    }

    if (FAILED(encoder->Commit())) {
        return false;
    }

    STATSTG stat = {};
    if (FAILED(memStream->Stat(&stat, STATFLAG_NONAME))) {
        return false;
    }

    const ULONGLONG streamSize = stat.cbSize.QuadPart;
    if (streamSize == 0 || streamSize > std::numeric_limits<ULONG>::max()) {
        return false;
    }

    LARGE_INTEGER seekZero = {};
    if (FAILED(memStream->Seek(seekZero, STREAM_SEEK_SET, nullptr))) {
        return false;
    }

    try {
        outPng.resize(static_cast<size_t>(streamSize));
    } catch (...) {
        return false;
    }

    ULONG bytesRead = 0;
    if (FAILED(memStream->Read(outPng.data(), static_cast<ULONG>(streamSize), &bytesRead)) ||
        bytesRead != static_cast<ULONG>(streamSize)) {
        outPng.clear();
        return false;
    }

    outW = static_cast<uint32_t>(w);
    outH = static_cast<uint32_t>(h);
    return true;
}

bool ImageCodecWic::TryDecodeImageBytesToPng(
    const std::vector<uint8_t>& rawImageBytes,
    RawClipboardPayload& outPayload,
    uint32_t maxDimension) {
    if (rawImageBytes.size() < 8 || !FitsWicBuffer(rawImageBytes.size()) || !EnsureWicFactory()) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory_->CreateStream(&stream))) {
        return false;
    }

    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(rawImageBytes.data()),
            static_cast<DWORD>(rawImageBytes.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromStream(
            stream.Get(),
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    ComPtr<IWICBitmapSource> sourceToEncode = frame;

    // 若指定了最大尺寸限制（如无字富文本提取首张大图做卡片预览），
    // 直接在 WIC 管线内降采样，避免将几千万像素的高清大图按原尺寸重编码为几十 MB 的 PNG 阻塞主线程
    if (maxDimension > 0) {
        UINT origW = 0;
        UINT origH = 0;
        if (SUCCEEDED(frame->GetSize(&origW, &origH)) &&
            origW > 0 && origH > 0 &&
            (origW > maxDimension || origH > maxDimension)) {
            UINT targetW = origW;
            UINT targetH = origH;
            if (origW >= origH) {
                targetW = maxDimension;
                targetH = std::max<UINT>(1U, static_cast<UINT>((static_cast<uint64_t>(origH) * maxDimension) / origW));
            } else {
                targetH = maxDimension;
                targetW = std::max<UINT>(1U, static_cast<UINT>((static_cast<uint64_t>(origW) * maxDimension) / origH));
            }

            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(wicFactory_->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame.Get(), targetW, targetH, WICBitmapInterpolationModeFant))) {
                sourceToEncode = scaler;
            }
        }
    }

    uint32_t w = 0;
    uint32_t h = 0;
    if (!EncodeBitmapSourceToPng(sourceToEncode.Get(), outPayload.pngBytes, w, h)) {
        return false;
    }

    outPayload.imageWidth = w;
    outPayload.imageHeight = h;
    return true;
}

bool ImageCodecWic::DecodePngToDib(
    const std::vector<uint8_t>& pngBytes,
    HGLOBAL& outDibGlobal) {
    outDibGlobal = nullptr;

    if (!FitsWicBuffer(pngBytes.size()) || !EnsureWicFactory()) {
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory_->CreateStream(&stream))) {
        return false;
    }

    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(pngBytes.data()),
            static_cast<DWORD>(pngBytes.size())))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromStream(
            stream.Get(),
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) ||
        width == 0 || height == 0 ||
        width > kMaxImageDim || height > kMaxImageDim) {
        return false;
    }

    const size_t stride = static_cast<size_t>(width) * 4ULL;
    const size_t imageBytes = stride * static_cast<size_t>(height);

    if (!FitsWicBuffer(imageBytes)) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(&converter))) {
        return false;
    }

    if (FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        return false;
    }

    std::vector<uint8_t> topDownPixels;
    try {
        topDownPixels.resize(imageBytes);
    } catch (...) {
        return false;
    }

    if (FAILED(converter->CopyPixels(
            nullptr,
            static_cast<UINT>(stride),
            static_cast<UINT>(imageBytes),
            topDownPixels.data()))) {
        return false;
    }

    const size_t dibTotalSize = sizeof(BITMAPINFOHEADER) + imageBytes;
    HGLOBAL hDib = ::GlobalAlloc(GMEM_MOVEABLE, dibTotalSize);
    if (!hDib) {
        return false;
    }

    {
        ScopedGlobalLock lock(hDib);
        if (!lock) {
            ::GlobalFree(hDib);
            return false;
        }

        auto* dibBase = static_cast<uint8_t*>(lock.Get());
        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(dibBase);
        std::memset(bih, 0, sizeof(BITMAPINFOHEADER));
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = static_cast<LONG>(width);
        bih->biHeight = static_cast<LONG>(height);
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = static_cast<DWORD>(imageBytes);

        uint8_t* dibPixels = dibBase + sizeof(BITMAPINFOHEADER);
        for (UINT y = 0; y < height; ++y) {
            const uint8_t* srcRow = topDownPixels.data() + static_cast<size_t>(height - 1U - y) * stride;
            uint8_t* dstRow = dibPixels + static_cast<size_t>(y) * stride;
            std::memcpy(dstRow, srcRow, stride);
        }
    }

    outDibGlobal = hDib;
    return true;
}




