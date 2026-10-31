#include "ui/RendererD2D.h"
#include "platform/I18n.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {
    constexpr size_t kMaxThumbnailCacheSize  = 12;
    constexpr size_t kMaxTextLayoutCacheSize = 16;
    constexpr float kCardCornerRadius        = 8.0f;
    constexpr float kControlCornerRadius     = 6.0f;

    bool IsFontFamilyInstalled(IDWriteFactory* dwriteFactory, const wchar_t* familyName) {
        if (!dwriteFactory || !familyName) {
            return false;
        }
        ComPtr<IDWriteFontCollection> fontCollection;
        if (FAILED(dwriteFactory->GetSystemFontCollection(&fontCollection, FALSE)) || !fontCollection) {
            return false;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(fontCollection->FindFamilyName(familyName, &index, &exists))) {
            return false;
        }
        return exists != FALSE;
    }

    HRESULT CreateTextFormatWithFallback(
        IDWriteFactory* dwriteFactory,
        const wchar_t* primaryFamily,
        const wchar_t* fallbackFamily,
        DWRITE_FONT_WEIGHT weight,
        float fontSizeDip,
        IDWriteTextFormat** outFormat) {
        if (!dwriteFactory || !outFormat) {
            return E_INVALIDARG;
        }

        const wchar_t* chosenFamily = IsFontFamilyInstalled(dwriteFactory, primaryFamily)
            ? primaryFamily
            : fallbackFamily;
        const wchar_t* localeName = I18n::Get().localeName;

        HRESULT hr = dwriteFactory->CreateTextFormat(
            chosenFamily,
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSizeDip,
            localeName,
            outFormat);

        if (FAILED(hr) && chosenFamily != fallbackFamily) {
            hr = dwriteFactory->CreateTextFormat(
                fallbackFamily,
                nullptr,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                fontSizeDip,
                localeName,
                outFormat);
        }
        return hr;
    }
} // namespace

RendererD2D::RendererD2D() {
    thumbnailCache_.reserve(kMaxThumbnailCacheSize);
    textLayoutCache_.reserve(kMaxTextLayoutCacheSize);
}

RendererD2D::~RendererD2D() {
    ReleaseCachedBitmaps();
}

bool RendererD2D::Initialize(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    hwnd_ = hwnd;

    const UINT windowDpi = GetDpiForWindow(hwnd_);
    dpi_ = (windowDpi > 0) ? windowDpi : 96;

    if (!d2dFactory_) {
        D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
        options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),
            &options,
            reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
            hr = D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory),
                &options,
                reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                return false;
            }
        }
    }

    if (!dwriteFactory_) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }
    }

    if (!wicFactory_) {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wicFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            hr = CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory_.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                return false;
            }
        }
    }

    if (!CreateTextFormats()) {
        return false;
    }

    return EnsureRenderTarget();
}

bool RendererD2D::CreateTextFormats() {
    if (!dwriteFactory_) {
        return false;
    }

    textLayoutCache_.clear();

    const wchar_t* fallbackFont = I18n::Get().fallbackFontFamily;

    // 1. 顶部标题格式：14pt SemiBold
    HRESULT hr = CreateTextFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        14.0f,
        titleFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 2. 卡片正文格式：最多 3 行，超出省略号截断
    hr = CreateTextFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_NORMAL,
        13.0f,
        bodyTextFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }
    bodyTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    bodyTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    bodyTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    bodyTextFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 20.0f, 16.0f);

    bodyEllipsisSign_.Reset();
    hr = dwriteFactory_->CreateEllipsisTrimmingSign(
        bodyTextFormat_.Get(),
        bodyEllipsisSign_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr) && bodyEllipsisSign_) {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        bodyTextFormat_->SetTrimming(&trimming, bodyEllipsisSign_.Get());
    }

    // 3. 轻量文字按钮与空状态提示格式
    hr = CreateTextFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_NORMAL,
        12.0f,
        smallBtnFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }
    smallBtnFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    smallBtnFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    smallBtnFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 4. Fluent 图标格式
    hr = CreateTextFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe Fluent Icons",
        L"Segoe MDL2 Assets",
        DWRITE_FONT_WEIGHT_NORMAL,
        13.0f,
        iconFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

bool RendererD2D::EnsureRenderTarget() {
    if (renderTarget_) {
        return true;
    }
    if (!d2dFactory_ || !hwnd_ || !IsWindow(hwnd_)) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT widthPx = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    UINT heightPx = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        widthPx = static_cast<UINT>(std::lround(Layout::WINDOW_WIDTH * dpi_ / 96.0f));
        heightPx = static_cast<UINT>(std::lround(Layout::WINDOW_HEIGHT * dpi_ / 96.0f));
    }

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_),
        static_cast<float>(dpi_));

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_,
        D2D1::SizeU(widthPx, heightPx),
        D2D1_PRESENT_OPTIONS_NONE);

    ReleaseCachedBitmaps();
    solidBrush_.Reset();

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        rtProps,
        hwndProps,
        renderTarget_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !renderTarget_) {
        return false;
    }

    renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    hr = renderTarget_->CreateSolidColorBrush(
        Palette::TextPrimary,
        solidBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !solidBrush_) {
        return false;
    }

    return true;
}

void RendererD2D::OnResize(UINT widthPx, UINT heightPx, UINT dpi) {
    if (dpi > 0 && dpi != dpi_) {
        dpi_ = dpi;
        ReleaseCachedBitmaps();
    }
    if (!renderTarget_) {
        return;
    }

    renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));

    if (widthPx > 0 && heightPx > 0) {
        HRESULT hr = renderTarget_->Resize(D2D1::SizeU(widthPx, heightPx));
        if (hr == D2DERR_RECREATE_TARGET) {
            ReleaseCachedBitmaps();
            solidBrush_.Reset();
            renderTarget_.Reset();
        }
    }
}

void RendererD2D::ReleaseCachedBitmaps() {
    thumbnailCache_.clear();
    textLayoutCache_.clear();
}

float RendererD2D::CalculateTotalContentHeight(const ClipItems& items) const {
    return layout_.CalculateTotalContentHeight(items);
}

ID2D1Bitmap* RendererD2D::InsertThumbnailCache(uint64_t itemId, ComPtr<ID2D1Bitmap> bitmap) {
    const uint64_t currentTick = ++cacheAccessCounter_;
    if (thumbnailCache_.size() >= kMaxThumbnailCacheSize) {
        auto lruIt = std::min_element(
            thumbnailCache_.begin(),
            thumbnailCache_.end(),
            [](const CachedThumbnail& a, const CachedThumbnail& b) {
                return a.lastUsedTick < b.lastUsedTick;
            });
        // 缓存上限为 12，进入此分支时容器必非空。
        lruIt->itemId = itemId;
        lruIt->lastUsedTick = currentTick;
        lruIt->bitmap = std::move(bitmap);
        return lruIt->bitmap.Get();
    }

    thumbnailCache_.push_back(CachedThumbnail{
        itemId,
        currentTick,
        std::move(bitmap)
    });
    return thumbnailCache_.back().bitmap.Get();
}

ID2D1Bitmap* RendererD2D::GetOrLoadThumbnail(const ClipItem& item, const std::filesystem::path& imageDir) {
    if (!renderTarget_ || !wicFactory_ || item.id == 0) {
        return nullptr;
    }

    // 1. 命中 LRU 缓存（含正向位图缓存与失败负缓存）则刷新访问计数并直接返回
    for (auto& entry : thumbnailCache_) {
        if (entry.itemId == item.id) {
            entry.lastUsedTick = ++cacheAccessCounter_;
            return entry.bitmap.Get();
        }
    }

    // 2. 解析图片完整路径并通过 WIC 解码与降采样（任何失败均写入负缓存，防止每帧重复读盘）
    // StorageManager 仅生成/加载带安全相对文件名的图片项，不存在 ID.png 或绝对路径记录。
    if (item.imageFileName.empty()) {
        return InsertThumbnailCache(item.id, nullptr);
    }
    const std::filesystem::path fullPath = imageDir / item.imageFileName;
    std::error_code ec;
    if (fullPath.empty() || !std::filesystem::exists(fullPath, ec)) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        fullPath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        decoder.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !decoder) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !frame) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    UINT origWidth = 0;
    UINT origHeight = 0;
    hr = frame->GetSize(&origWidth, &origHeight);
    if (FAILED(hr) || origWidth == 0 || origHeight == 0) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    const float dpiScale = static_cast<float>(dpi_) / 96.0f;
    const UINT maxW = static_cast<UINT>(std::lround(300.0f * dpiScale));
    const UINT maxH = static_cast<UINT>(std::lround(150.0f * dpiScale));

    UINT targetWidth = origWidth;
    UINT targetHeight = origHeight;
    if (origWidth > maxW || origHeight > maxH) {
        const double scaleX = static_cast<double>(maxW) / static_cast<double>(origWidth);
        const double scaleY = static_cast<double>(maxH) / static_cast<double>(origHeight);
        const double scale = std::min(scaleX, scaleY);
        targetWidth = std::max<UINT>(1, static_cast<UINT>(std::lround(origWidth * scale)));
        targetHeight = std::max<UINT>(1, static_cast<UINT>(std::lround(origHeight * scale)));
    }

    ComPtr<IWICBitmapScaler> scaler;
    hr = wicFactory_->CreateBitmapScaler(scaler.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !scaler) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    hr = scaler->Initialize(
        frame.Get(),
        targetWidth,
        targetHeight,
        WICBitmapInterpolationModeFant);
    if (FAILED(hr)) {
        hr = scaler->Initialize(
            frame.Get(),
            targetWidth,
            targetHeight,
            WICBitmapInterpolationModeLinear);
        if (FAILED(hr)) {
            return InsertThumbnailCache(item.id, nullptr);
        }
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory_->CreateFormatConverter(converter.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !converter) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    hr = converter->Initialize(
        scaler.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_),
        static_cast<float>(dpi_));

    ComPtr<ID2D1Bitmap> d2dBitmap;
    hr = renderTarget_->CreateBitmapFromWicBitmap(
        converter.Get(),
        &bmpProps,
        d2dBitmap.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !d2dBitmap) {
        return InsertThumbnailCache(item.id, nullptr);
    }

    // 3. 写入严格上限 12 项的 LRU 缓存
    return InsertThumbnailCache(item.id, std::move(d2dBitmap));
}

IDWriteTextLayout* RendererD2D::InsertTextLayoutCache(
    uint64_t itemId,
    float maxWidth,
    float maxHeight,
    ComPtr<IDWriteTextLayout> textLayout) {
    const uint64_t currentTick = ++cacheAccessCounter_;
    if (textLayoutCache_.size() >= kMaxTextLayoutCacheSize) {
        auto lruIt = std::min_element(
            textLayoutCache_.begin(),
            textLayoutCache_.end(),
            [](const CachedTextLayout& a, const CachedTextLayout& b) {
                return a.lastUsedTick < b.lastUsedTick;
            });
        // 缓存上限为 16，进入此分支时容器必非空。
        lruIt->itemId = itemId;
        lruIt->maxWidth = maxWidth;
        lruIt->maxHeight = maxHeight;
        lruIt->lastUsedTick = currentTick;
        lruIt->textLayout = std::move(textLayout);
        return lruIt->textLayout.Get();
    }

    textLayoutCache_.push_back(CachedTextLayout{
        itemId,
        maxWidth,
        maxHeight,
        currentTick,
        std::move(textLayout)
    });
    return textLayoutCache_.back().textLayout.Get();
}

IDWriteTextLayout* RendererD2D::GetOrCreateTextLayout(
    const ClipItem& item,
    float maxWidth,
    float maxHeight) {
    if (!dwriteFactory_ || !bodyTextFormat_ || item.id == 0 || item.previewText.empty()) {
        return nullptr;
    }

    for (auto& entry : textLayoutCache_) {
        if (entry.itemId == item.id &&
            std::abs(entry.maxWidth - maxWidth) < 0.1f &&
            std::abs(entry.maxHeight - maxHeight) < 0.1f) {
            entry.lastUsedTick = ++cacheAccessCounter_;
            return entry.textLayout.Get();
        }
    }

    const UINT32 textLen = static_cast<UINT32>(
        std::min<size_t>(item.previewText.size(), 512));

    ComPtr<IDWriteTextLayout> textLayout;
    const HRESULT hr = dwriteFactory_->CreateTextLayout(
        item.previewText.data(),
        textLen,
        bodyTextFormat_.Get(),
        maxWidth,
        maxHeight,
        textLayout.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !textLayout) {
        return nullptr;
    }

    return InsertTextLayoutCache(item.id, maxWidth, maxHeight, std::move(textLayout));
}

void RendererD2D::Render(const ClipItems& items, const UIViewState& state, const std::filesystem::path& imageDir) {
    if (!EnsureRenderTarget() || !solidBrush_) {
        return;
    }

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    renderTarget_->Clear(Palette::WindowBg);

    RenderTopBar(state);
    RenderNavBar(items, state);
    RenderVirtualList(items, state, imageDir);
    RenderActionTooltip(items, state);

    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseCachedBitmaps();
        solidBrush_.Reset();
        renderTarget_.Reset();
    }
}

void RendererD2D::RenderTopBar(const UIViewState& state) {
    // 左侧标题 (x = 14.0f, y = 2.0f ~ 40.0f，与下方导航胶囊及卡片视觉网格对齐)
    const wchar_t* titleText = I18n::Get().appTitle;
    const D2D1_RECT_F titleRect = D2D1::RectF(14.0f, 2.0f, 240.0f, Layout::TOP_BAR_HEIGHT);
    solidBrush_->SetColor(Palette::TextPrimary);
    renderTarget_->DrawText(
        titleText,
        static_cast<UINT32>(std::wcslen(titleText)),
        titleFormat_.Get(),
        titleRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE);

    // 右侧关闭按钮 (x = 320.0f ~ 348.0f)
    const bool closeHovered = (state.hoveredRegion == HitRegionType::CloseButton);
    if (closeHovered) {
        solidBrush_->SetColor(Palette::CloseHoverBg);
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(kCloseBtnRect, kControlCornerRadius, kControlCornerRadius),
            solidBrush_.Get());
    }

    solidBrush_->SetColor(closeHovered ? Palette::DangerRed : Palette::TextSecondary);
    renderTarget_->DrawText(
        FluentIcons::Close,
        static_cast<UINT32>(std::wcslen(FluentIcons::Close)),
        iconFormat_.Get(),
        kCloseBtnRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void RendererD2D::RenderNavBar(const ClipItems& items, const UIViewState& state) {
    const bool isHistoryActive = (state.activeTab == ActiveTab::History);

    // 1. 左侧胶囊分段控制器底座轨道 (Segmented Pill Track)
    solidBrush_->SetColor(Palette::NavTrackBg);
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(kNavSegmentTrackRect, 7.0f, 7.0f),
        solidBrush_.Get());

    auto drawSegmentedTab = [&](const D2D1_RECT_F& rect, const wchar_t* glyph, bool isActive, bool isHovered) {
        if (isActive) {
            const D2D1_RECT_F shadowRect = D2D1::RectF(rect.left, rect.top + 1.0f, rect.right, rect.bottom + 1.0f);
            solidBrush_->SetColor(Palette::CardShadow);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(shadowRect, 5.5f, 5.5f),
                solidBrush_.Get());

            const D2D1_ROUNDED_RECT activePill = D2D1::RoundedRect(rect, 5.5f, 5.5f);
            solidBrush_->SetColor(Palette::NavActivePillBg);
            renderTarget_->FillRoundedRectangle(activePill, solidBrush_.Get());

            solidBrush_->SetColor(Palette::NavActivePillBorder);
            renderTarget_->DrawRoundedRectangle(activePill, solidBrush_.Get(), 0.75f);
        } else if (isHovered) {
            solidBrush_->SetColor(Palette::SubtleHoverFill);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(rect, 5.5f, 5.5f),
                solidBrush_.Get());
        }

        solidBrush_->SetColor(isActive ? Palette::AccentBlue : Palette::TextSecondary);
        renderTarget_->DrawText(
            glyph,
            static_cast<UINT32>(std::wcslen(glyph)),
            iconFormat_.Get(),
            rect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
    };

    drawSegmentedTab(
        kTabHistoryRect,
        FluentIcons::HistoryTab,
        isHistoryActive,
        state.hoveredRegion == HitRegionType::TabHistory);

    drawSegmentedTab(
        kTabPinnedRect,
        FluentIcons::PinUnpinned,
        !isHistoryActive,
        state.hoveredRegion == HitRegionType::TabPinned);

    // 2. 右侧 "全部清除" 按钮 (x = 282 ~ 348)
    const bool hasItems = !items.empty();
    const bool clearHovered = hasItems && (state.hoveredRegion == HitRegionType::ClearAllButton);
    if (clearHovered) {
        solidBrush_->SetColor(Palette::SubtleHoverFill);
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(kClearAllBtnRect, kControlCornerRadius, kControlCornerRadius),
            solidBrush_.Get());
    }

    const wchar_t* clearAllText = I18n::Get().clearAll;
    solidBrush_->SetColor(hasItems
        ? (clearHovered ? Palette::TextPrimary : Palette::TextSecondary)
        : Palette::TextMuted);
    renderTarget_->DrawText(
        clearAllText,
        static_cast<UINT32>(std::wcslen(clearAllText)),
        smallBtnFormat_.Get(),
        kClearAllBtnRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE);

    // 3. 仅当列表发生向下滚动时才浮现极细分隔线，保持顶部与卡片列表之间的呼吸感
    if (state.scrollOffsetY > 1.0f) {
        solidBrush_->SetColor(Palette::DividerLine);
        renderTarget_->DrawLine(
            D2D1::Point2F(Layout::LIST_PAD_HORZ, kViewportTopDip - 0.5f),
            D2D1::Point2F(Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ, kViewportTopDip - 0.5f),
            solidBrush_.Get(),
            1.0f);
    }
}

void RendererD2D::RenderVirtualList(
    const ClipItems& items,
    const UIViewState& state,
    const std::filesystem::path& imageDir) {
    const D2D1_RECT_F viewportRect = D2D1::RectF(
        0.0f,
        kViewportTopDip,
        Layout::WINDOW_WIDTH,
        Layout::WINDOW_HEIGHT);

    renderTarget_->PushAxisAlignedClip(viewportRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (items.empty()) {
        // 当处于主历史记录视图且列表已全部清空时，顺带清空显存中的缩略图缓存、文本布局缓存与虚拟列表缓存
        if (state.activeTab == ActiveTab::History) {
            thumbnailCache_.clear();
            textLayoutCache_.clear();
        }
        layout_.EnsureLayoutCache(items);

        const wchar_t* emptyHintText = I18n::Get().emptyHint;
        solidBrush_->SetColor(Palette::TextMuted);
        renderTarget_->DrawText(
            emptyHintText,
            static_cast<UINT32>(std::wcslen(emptyHintText)),
            smallBtnFormat_.Get(),
            viewportRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
        renderTarget_->PopAxisAlignedClip();
        return;
    }

    layout_.EnsureLayoutCache(items);

    const float viewStart = std::max(0.0f, state.scrollOffsetY);
    const float viewEnd = viewStart + Layout::VIEWPORT_HEIGHT;

    const size_t startIndex = layout_.FindFirstVisibleCandidateIndex(viewStart);

    for (size_t i = startIndex; i < items.size(); ++i) {
        const float itemTopY = layout_.GetItemTopY(i);
        if (itemTopY >= viewEnd) {
            break;
        }

        const ClipItem& item = items[i];
        const float cardHeight = GetClipCardHeight(item);
        const float itemBottomY = itemTopY + cardHeight;

        // 严格虚拟列表剔除：仅当卡片区间与 [viewStart, viewEnd] 相交时才绘制
        if (itemBottomY > viewStart) {
            const float screenCardTop = kViewportTopDip + (itemTopY - viewStart);
            RenderSingleCard(item, screenCardTop, cardHeight, state, imageDir);
        }
    }

    if (state.showDropIndicator) {
        const float indicatorScreenY = kViewportTopDip + (state.dropIndicatorY - viewStart);
        if (indicatorScreenY >= kViewportTopDip && indicatorScreenY <= Layout::WINDOW_HEIGHT) {
            const float lineLeft = Layout::LIST_PAD_HORZ + 12.0f;
            const float lineRight = Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ - 12.0f;
            const D2D1_RECT_F barRect = D2D1::RectF(
                lineLeft,
                indicatorScreenY - 1.0f,
                lineRight,
                indicatorScreenY + 1.0f);
            solidBrush_->SetColor(D2D1::ColorF(0x00 / 255.0f, 0x67 / 255.0f, 0xC0 / 255.0f, 0.72f));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(barRect, 1.0f, 1.0f), solidBrush_.Get());
        }
    }

    const float totalContentHeight = layout_.GetCachedTotalHeight();
    if (totalContentHeight > Layout::VIEWPORT_HEIGHT) {
        RenderScrollbar(totalContentHeight, viewStart);
    }

    renderTarget_->PopAxisAlignedClip();
}

void RendererD2D::RenderSingleCard(
    const ClipItem& item,
    float cardTop,
    float cardHeight,
    const UIViewState& state,
    const std::filesystem::path& imageDir) {
    const float cardLeft = Layout::LIST_PAD_HORZ;
    const float cardRight = Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ;
    const float cardBottom = cardTop + cardHeight;
    const D2D1_RECT_F cardRect = D2D1::RectF(cardLeft, cardTop, cardRight, cardBottom);
    const D2D1_ROUNDED_RECT cardRoundedRect = D2D1::RoundedRect(cardRect, kCardCornerRadius, kCardCornerRadius);

    const bool isCardSelected = (state.selectedItemId != 0 && state.selectedItemId == item.id);
    const bool isCardHovered = (state.hoveredItemId == item.id);
    const bool isBodyHovered = isCardHovered && (state.hoveredRegion == HitRegionType::CardBody);
    const bool isImage = IsImageClipItem(item);

    // 1. 卡片底部 1.0f DIP 零开销微投影，增强白色卡片在浅灰画布上的悬浮感
    const D2D1_RECT_F shadowRect = D2D1::RectF(cardLeft, cardTop + 1.0f, cardRight, cardBottom + 1.0f);
    solidBrush_->SetColor(Palette::CardShadow);
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(shadowRect, kCardCornerRadius, kCardCornerRadius),
        solidBrush_.Get());

    // 2. 绘制卡片主体背景（选中项使用极淡冰蓝微染色底，取代突兀粗蓝框）
    D2D1_COLOR_F cardFillColor = Palette::CardBg;
    if (isCardSelected) {
        cardFillColor = isBodyHovered ? Palette::CardSelectedHoverBg : Palette::CardSelectedBg;
    } else if (isCardHovered) {
        cardFillColor = Palette::CardHoverBg;
    }
    solidBrush_->SetColor(cardFillColor);
    renderTarget_->FillRoundedRectangle(cardRoundedRect, solidBrush_.Get());

    // 3. 选中态左侧竖向圆角胶囊指示条 (Fluent 2 Pill Indicator, 3.0 x 22.0 DIP)
    if (isCardSelected) {
        const float midY = (cardTop + cardBottom) * 0.5f;
        const float pillHalfHeight = 11.0f;
        const D2D1_RECT_F accentPillRect = D2D1::RectF(
            cardLeft + 3.5f,
            midY - pillHalfHeight,
            cardLeft + 6.5f,
            midY + pillHalfHeight);
        solidBrush_->SetColor(Palette::AccentBlue);
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(accentPillRect, 1.5f, 1.5f),
            solidBrush_.Get());
    }

    // 4. 卡片主体左侧内容区（宽度 = 卡片宽 - 右侧操作区 32.0f DIP）
    const float actionAreaLeft = cardRight - Layout::CARD_ACTION_WIDTH;
    const D2D1_RECT_F contentRect = D2D1::RectF(
        cardLeft + 14.0f,
        cardTop + 8.0f,
        actionAreaLeft - 4.0f,
        cardBottom - 8.0f);

    const float contentWidth = std::max(1.0f, contentRect.right - contentRect.left);
    const float contentHeight = std::max(1.0f, contentRect.bottom - contentRect.top);

    if (isImage) {
        ID2D1Bitmap* thumbnail = GetOrLoadThumbnail(item, imageDir);
        if (thumbnail) {
            const D2D1_SIZE_F bmpSize = thumbnail->GetSize();
            if (bmpSize.width > 0.0f && bmpSize.height > 0.0f) {
                const float scaleX = contentWidth / bmpSize.width;
                const float scaleY = contentHeight / bmpSize.height;
                const float scale = std::min(scaleX, scaleY);

                const float drawW = bmpSize.width * scale;
                const float drawH = bmpSize.height * scale;
                const float drawLeft = contentRect.left + (contentWidth - drawW) * 0.5f;
                const float drawTop = contentRect.top + (contentHeight - drawH) * 0.5f;
                const D2D1_RECT_F destRect = D2D1::RectF(
                    drawLeft,
                    drawTop,
                    drawLeft + drawW,
                    drawTop + drawH);

                renderTarget_->DrawBitmap(
                    thumbnail,
                    destRect,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

                solidBrush_->SetColor(Palette::DividerLine);
                renderTarget_->DrawRoundedRectangle(
                    D2D1::RoundedRect(destRect, 4.0f, 4.0f),
                    solidBrush_.Get(),
                    0.75f);
            }
        } else {
            solidBrush_->SetColor(Palette::ImagePlaceholder);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(contentRect, 4.0f, 4.0f),
                solidBrush_.Get());

            const wchar_t* imgPlaceholderText = I18n::Get().imgPreviewUnavailable;
            solidBrush_->SetColor(Palette::TextMuted);
            renderTarget_->DrawText(
                imgPlaceholderText,
                static_cast<UINT32>(std::wcslen(imgPlaceholderText)),
                smallBtnFormat_.Get(),
                contentRect,
                solidBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    } else if (IsMixedPreviewClipItem(item)) {
        // 短图文混排项（文字不足以填满 1 倍高度且带图片）：2 倍高度，左文右图展示
        constexpr float kMixedColumnGap = 10.0f;
        const float halfWidth = std::max(1.0f, (contentWidth - kMixedColumnGap) * 0.5f);

        const D2D1_RECT_F leftTextRect = D2D1::RectF(
            contentRect.left,
            contentRect.top,
            contentRect.left + halfWidth,
            contentRect.bottom);

        const D2D1_RECT_F rightImageRect = D2D1::RectF(
            contentRect.left + halfWidth + kMixedColumnGap,
            contentRect.top,
            contentRect.right,
            contentRect.bottom);

        const std::wstring& text = item.previewText;
        if (!text.empty()) {
            solidBrush_->SetColor(Palette::TextPrimary);
            if (IDWriteTextLayout* textLayout = GetOrCreateTextLayout(item, halfWidth, contentHeight)) {
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(leftTextRect.left, leftTextRect.top),
                    textLayout,
                    solidBrush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            } else if (bodyTextFormat_) {
                const UINT32 textLen = static_cast<UINT32>(
                    std::min<size_t>(text.size(), 512));
                renderTarget_->DrawText(
                    text.data(),
                    textLen,
                    bodyTextFormat_.Get(),
                    leftTextRect,
                    solidBrush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        const float imgBoxW = std::max(1.0f, rightImageRect.right - rightImageRect.left);
        const float imgBoxH = std::max(1.0f, rightImageRect.bottom - rightImageRect.top);
        ID2D1Bitmap* thumbnail = GetOrLoadThumbnail(item, imageDir);
        if (thumbnail) {
            const D2D1_SIZE_F bmpSize = thumbnail->GetSize();
            if (bmpSize.width > 0.0f && bmpSize.height > 0.0f) {
                const float scaleX = imgBoxW / bmpSize.width;
                const float scaleY = imgBoxH / bmpSize.height;
                const float scale = std::min(scaleX, scaleY);

                const float drawW = bmpSize.width * scale;
                const float drawH = bmpSize.height * scale;
                const float drawLeft = rightImageRect.left + (imgBoxW - drawW) * 0.5f;
                const float drawTop = rightImageRect.top + (imgBoxH - drawH) * 0.5f;
                const D2D1_RECT_F destRect = D2D1::RectF(
                    drawLeft,
                    drawTop,
                    drawLeft + drawW,
                    drawTop + drawH);

                renderTarget_->DrawBitmap(
                    thumbnail,
                    destRect,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

                solidBrush_->SetColor(Palette::DividerLine);
                renderTarget_->DrawRoundedRectangle(
                    D2D1::RoundedRect(destRect, 4.0f, 4.0f),
                    solidBrush_.Get(),
                    1.0f);
            }
        } else {
            solidBrush_->SetColor(Palette::ImagePlaceholder);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(rightImageRect, 4.0f, 4.0f),
                solidBrush_.Get());

            const wchar_t* imgPlaceholderText = I18n::Get().imgPreviewUnavailable;
            solidBrush_->SetColor(Palette::TextMuted);
            renderTarget_->DrawText(
                imgPlaceholderText,
                static_cast<UINT32>(std::wcslen(imgPlaceholderText)),
                smallBtnFormat_.Get(),
                rightImageRect,
                solidBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    } else {
        // 文本/富文本项：若 previewText 为空（如空表格等“都无留白”卡片），直接留白不绘制任何占位文字
        const std::wstring& text = item.previewText;
        if (!text.empty()) {
            solidBrush_->SetColor(Palette::TextPrimary);
            if (IDWriteTextLayout* textLayout = GetOrCreateTextLayout(item, contentWidth, contentHeight)) {
                renderTarget_->DrawTextLayout(
                    D2D1::Point2F(contentRect.left, contentRect.top),
                    textLayout,
                    solidBrush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            } else if (bodyTextFormat_) {
                const UINT32 textLen = static_cast<UINT32>(
                    std::min<size_t>(text.size(), 512));
                renderTarget_->DrawText(
                    text.data(),
                    textLen,
                    bodyTextFormat_.Get(),
                    contentRect,
                    solidBrush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    // 5. 卡片右侧操作区（宽 32.0f DIP）：
    //    默认隐藏次要操作图标以保持极简；鼠标悬停卡片时直接展示 [右上：固定] [右中：粘贴为纯文本] [右下：删除]，
    //    若卡片处于已固定状态，未悬停时仅在右上角展示柔和图钉状态标记。
    if (isCardHovered || item.isPinned) {
        const D2D1_RECT_F pinBtnRect = GetCardPinBtnRect(cardTop);
        const bool isPinHovered = isCardHovered && (state.hoveredRegion == HitRegionType::CardPinButton);
        if (isPinHovered) {
            solidBrush_->SetColor(Palette::ActionHoverFill);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(pinBtnRect, 5.0f, 5.0f),
                solidBrush_.Get());
        }
        const wchar_t* pinGlyph = item.isPinned ? FluentIcons::PinPinned : FluentIcons::PinUnpinned;
        if (item.isPinned) {
            solidBrush_->SetColor(isCardHovered ? Palette::AccentBlue : Palette::PinnedBadgeIdle);
        } else {
            solidBrush_->SetColor(isPinHovered ? Palette::TextPrimary : Palette::TextSecondary);
        }
        renderTarget_->DrawText(
            pinGlyph,
            static_cast<UINT32>(std::wcslen(pinGlyph)),
            iconFormat_.Get(),
            pinBtnRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    if (isCardHovered) {
        if (CanPlainTextPaste(item)) {
            const D2D1_RECT_F plainBtnRect = GetCardPlainPasteBtnRect(cardTop);
            const bool isPlainHovered = (state.hoveredRegion == HitRegionType::CardPlainPasteButton);
            if (isPlainHovered) {
                solidBrush_->SetColor(Palette::AccentBlueSubtle);
                renderTarget_->FillRoundedRectangle(
                    D2D1::RoundedRect(plainBtnRect, 5.0f, 5.0f),
                    solidBrush_.Get());
            }
            solidBrush_->SetColor(isPlainHovered ? Palette::AccentBlue : Palette::TextSecondary);
            renderTarget_->DrawText(
                FluentIcons::PlainText,
                static_cast<UINT32>(std::wcslen(FluentIcons::PlainText)),
                iconFormat_.Get(),
                plainBtnRect,
                solidBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        const D2D1_RECT_F deleteBtnRect = GetCardDeleteBtnRect(cardTop, cardHeight);
        const bool isDeleteHovered = (state.hoveredRegion == HitRegionType::CardDeleteButton);
        if (isDeleteHovered) {
            solidBrush_->SetColor(Palette::SlideDeleteHover);
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(deleteBtnRect, 5.0f, 5.0f),
                solidBrush_.Get());
        }
        solidBrush_->SetColor(isDeleteHovered ? Palette::DangerRed : Palette::TextSecondary);
        renderTarget_->DrawText(
            FluentIcons::DeleteTrash,
            static_cast<UINT32>(std::wcslen(FluentIcons::DeleteTrash)),
            iconFormat_.Get(),
            deleteBtnRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // 6. 卡片圆角外框描边（统一 1.0f DIP 细腻描边：拖动排序、键盘选中或悬停/常规边框）
    const bool isDraggingThisCard = (state.draggingItemId != 0 && state.draggingItemId == item.id);
    if (isDraggingThisCard) {
        solidBrush_->SetColor(D2D1::ColorF(0x00 / 255.0f, 0x67 / 255.0f, 0xC0 / 255.0f, 0.55f));
        renderTarget_->DrawRoundedRectangle(cardRoundedRect, solidBrush_.Get(), 1.0f);
    } else if (isCardSelected) {
        solidBrush_->SetColor(Palette::CardSelectedBorder);
        renderTarget_->DrawRoundedRectangle(cardRoundedRect, solidBrush_.Get(), 1.0f);
    } else {
        solidBrush_->SetColor(isCardHovered ? Palette::CardHoverBorder : Palette::CardBorder);
        renderTarget_->DrawRoundedRectangle(cardRoundedRect, solidBrush_.Get(), 1.0f);
    }
}

void RendererD2D::RenderScrollbar(float totalContentHeight, float scrollOffsetY) {
    const ScrollbarMetrics metrics = CalculateScrollbarMetrics(totalContentHeight, scrollOffsetY);
    if (!metrics.isVisible) {
        return;
    }

    const D2D1_RECT_F thumbRect = D2D1::RectF(
        Layout::WINDOW_WIDTH - 6.0f,
        metrics.thumbTop,
        Layout::WINDOW_WIDTH - 2.0f,
        metrics.thumbBottom);

    solidBrush_->SetColor(Palette::ScrollbarThumb);
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(thumbRect, 2.0f, 2.0f),
        solidBrush_.Get());
}

void RendererD2D::RenderActionTooltip(const ClipItems& items, const UIViewState& state) {
    if (state.draggingItemId != 0 || state.hoveredItemId == 0 || !dwriteFactory_ || !smallBtnFormat_) {
        return;
    }

    if (state.hoveredRegion != HitRegionType::CardPinButton &&
        state.hoveredRegion != HitRegionType::CardPlainPasteButton &&
        state.hoveredRegion != HitRegionType::CardDeleteButton) {
        return;
    }

    layout_.EnsureLayoutCache(items);

    size_t targetIndex = items.size();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == state.hoveredItemId) {
            targetIndex = i;
            break;
        }
    }
    if (targetIndex >= items.size()) {
        return;
    }

    const ClipItem& item = items[targetIndex];
    const float viewStart = std::max(0.0f, state.scrollOffsetY);
    const float cardTop = kViewportTopDip + (layout_.GetItemTopY(targetIndex) - viewStart);
    const float cardHeight = GetClipCardHeight(item);

    D2D1_RECT_F btnRect{};
    const wchar_t* tipText = nullptr;

    switch (state.hoveredRegion) {
    case HitRegionType::CardPinButton:
        btnRect = GetCardPinBtnRect(cardTop);
        tipText = item.isPinned ? I18n::Get().unpinItem : I18n::Get().pinItem;
        break;
    case HitRegionType::CardPlainPasteButton:
        if (!CanPlainTextPaste(item)) {
            return;
        }
        btnRect = GetCardPlainPasteBtnRect(cardTop);
        tipText = I18n::Get().plainTextPaste;
        break;
    case HitRegionType::CardDeleteButton:
        btnRect = GetCardDeleteBtnRect(cardTop, cardHeight);
        tipText = I18n::Get().deleteItem;
        break;
    default:
        return;
    }

    if (!tipText || tipText[0] == L'\0') {
        return;
    }

    if (btnRect.bottom <= kViewportTopDip || btnRect.top >= Layout::WINDOW_HEIGHT) {
        return;
    }

    const UINT32 textLen = static_cast<UINT32>(std::wcslen(tipText));
    ComPtr<IDWriteTextLayout> tipLayout;
    if (FAILED(dwriteFactory_->CreateTextLayout(
            tipText,
            textLen,
            smallBtnFormat_.Get(),
            240.0f,
            24.0f,
            tipLayout.GetAddressOf())) || !tipLayout) {
        return;
    }

    DWRITE_TEXT_METRICS textMetrics{};
    if (FAILED(tipLayout->GetMetrics(&textMetrics))) {
        return;
    }

    constexpr float kPadHorz = 8.0f;
    constexpr float kTooltipHeight = 22.0f;
    constexpr float kTooltipRadius = 5.0f;
    const float tooltipWidth = std::ceil(textMetrics.widthIncludingTrailingWhitespace) + kPadHorz * 2.0f;

    const float tooltipRight = btnRect.left - 6.0f;
    const float tooltipLeft = std::max(Layout::LIST_PAD_HORZ + 4.0f, tooltipRight - tooltipWidth);
    const float centerY = (btnRect.top + btnRect.bottom) * 0.5f;
    const float tooltipTop = std::clamp(
        centerY - kTooltipHeight * 0.5f,
        kViewportTopDip + 2.0f,
        Layout::WINDOW_HEIGHT - kTooltipHeight - 2.0f);
    const float tooltipBottom = tooltipTop + kTooltipHeight;

    const D2D1_RECT_F tooltipRect = D2D1::RectF(tooltipLeft, tooltipTop, tooltipRight, tooltipBottom);
    const D2D1_RECT_F shadowRect = D2D1::RectF(tooltipLeft, tooltipTop + 1.5f, tooltipRight, tooltipBottom + 1.5f);

    solidBrush_->SetColor(Palette::TooltipShadow);
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(shadowRect, kTooltipRadius, kTooltipRadius),
        solidBrush_.Get());

    const D2D1_ROUNDED_RECT roundedTooltip = D2D1::RoundedRect(tooltipRect, kTooltipRadius, kTooltipRadius);
    solidBrush_->SetColor(Palette::TooltipBg);
    renderTarget_->FillRoundedRectangle(roundedTooltip, solidBrush_.Get());

    solidBrush_->SetColor(Palette::TooltipBorder);
    renderTarget_->DrawRoundedRectangle(roundedTooltip, solidBrush_.Get(), 0.75f);

    solidBrush_->SetColor(Palette::TooltipText);
    renderTarget_->DrawText(
        tipText,
        textLen,
        smallBtnFormat_.Get(),
        tooltipRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

HitTestResult RendererD2D::HitTest(
    float dipX,
    float dipY,
    const ClipItems& items,
    const UIViewState& state) const {
    return layout_.HitTest(dipX, dipY, items, state);
}







