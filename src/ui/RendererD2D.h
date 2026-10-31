#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include "ui/ThemeAndLayout.h"
#include "ui/VirtualListLayout.h"
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

class RendererD2D {
public:
    RendererD2D();
    ~RendererD2D();

    bool Initialize(HWND hwnd);
    void OnResize(UINT widthPx, UINT heightPx, UINT dpi);

    // 虚拟化渲染整个窗口
    void Render(const ClipItems& items, const UIViewState& state, const std::filesystem::path& imageDir);

    // 命中测试（传入坐标已转换为 DIP）
    HitTestResult HitTest(float dipX, float dipY, const ClipItems& items, const UIViewState& state) const;

    // 计算固定页拖动排序的目标插入槽位
    DropTargetSlot CalculateDropSlot(
        float dipY,
        uint64_t draggingItemId,
        const ClipItems& items,
        const UIViewState& state) const {
        return layout_.CalculateDropSlot(dipY, draggingItemId, items, state);
    }

    // 显式失效布局高度缓存
    void InvalidateLayoutCache() const noexcept {
        layout_.InvalidateCache();
    }

    // 计算列表总内容高度（DIP），用于滚动边界钳制
    float CalculateTotalContentHeight(const ClipItems& items) const;

    // 查询指定卡片在内容坐标系中的上下边界（DIP）
    bool GetItemVerticalBounds(
        uint64_t itemId,
        const ClipItems& items,
        float& outTopY,
        float& outBottomY) const {
        return layout_.GetItemVerticalBounds(itemId, items, outTopY, outBottomY);
    }

    // 当窗口隐藏时调用，立即清空缓存的缩略图 ID2D1Bitmap，将后台内存降至最低
    void ReleaseCachedBitmaps();

private:
    struct CachedThumbnail {
        uint64_t itemId = 0;
        uint64_t lastUsedTick = 0;
        ComPtr<ID2D1Bitmap> bitmap;
    };

    struct CachedTextLayout {
        uint64_t itemId = 0;
        float maxWidth = 0.0f;
        float maxHeight = 0.0f;
        uint64_t lastUsedTick = 0;
        ComPtr<IDWriteTextLayout> textLayout;
    };

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;

    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> bodyTextFormat_;
    ComPtr<IDWriteTextFormat> smallBtnFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;

    ComPtr<ID2D1SolidColorBrush> solidBrush_;
    ComPtr<IDWriteInlineObject> bodyEllipsisSign_;
    uint64_t cacheAccessCounter_ = 0;

    std::vector<CachedThumbnail> thumbnailCache_;   // 严格限制最多 12 个元素的 LRU 缓存
    std::vector<CachedTextLayout> textLayoutCache_; // 严格限制最多 16 个元素的视口文本布局 LRU 缓存
    VirtualListLayout layout_;

    bool CreateTextFormats();
    bool EnsureRenderTarget();
    ID2D1Bitmap* GetOrLoadThumbnail(const ClipItem& item, const std::filesystem::path& imageDir);
    ID2D1Bitmap* InsertThumbnailCache(uint64_t itemId, ComPtr<ID2D1Bitmap> bitmap); // 空位图表示加载失败的负缓存
    IDWriteTextLayout* GetOrCreateTextLayout(const ClipItem& item, float maxWidth, float maxHeight);
    IDWriteTextLayout* InsertTextLayoutCache(uint64_t itemId, float maxWidth, float maxHeight, ComPtr<IDWriteTextLayout> textLayout);

    void RenderTopBar(const UIViewState& state);
    void RenderNavBar(const ClipItems& items, const UIViewState& state);
    void RenderVirtualList(const ClipItems& items, const UIViewState& state, const std::filesystem::path& imageDir);
    void RenderSingleCard(const ClipItem& item, float cardTop, float cardHeight, const UIViewState& state, const std::filesystem::path& imageDir);
    void RenderScrollbar(float totalContentHeight, float scrollOffsetY);
    void RenderActionTooltip(const ClipItems& items, const UIViewState& state);
};
