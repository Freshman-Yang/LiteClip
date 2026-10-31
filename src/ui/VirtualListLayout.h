#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include "ui/ThemeAndLayout.h"
#include <d2d1.h>
#include <algorithm>
#include <deque>

enum class HitRegionType : uint8_t {
    None = 0,
    TopBarDragArea,
    CloseButton,
    TabHistory,
    TabPinned,
    ClearAllButton,
    CardBody,
    CardPlainPasteButton,
    CardDeleteButton,
    CardPinButton,
    Scrollbar
};

struct HitTestResult {
    HitRegionType region = HitRegionType::None;
    uint64_t itemId = 0;
};

struct DropTargetSlot {
    size_t slotIndex = 0;           // 插入槽位 [0, items.size()]
    size_t targetItemIndex = 0;     // 移动后的最终元素索引 [0, items.size() - 1]
    float indicatorContentY = 0.0f; // 插入指示线在内容坐标系中的 Y 坐标（DIP）
    bool isEffectiveMove = false;   // 是否实际改变位置（若落在原位置前后槽位则为 false）
};

struct UIViewState {
    ActiveTab activeTab = ActiveTab::History;
    float scrollOffsetY = 0.0f;
    uint64_t selectedItemId = 0;     // 当前键盘选中的卡片 ID（0 表示无）
    uint64_t hoveredItemId = 0;
    HitRegionType hoveredRegion = HitRegionType::None;
    uint64_t draggingItemId = 0;     // 当前正在固定页拖动排序的卡片 ID（0 表示无）
    bool showDropIndicator = false;  // 是否在列表内显示拖动排序目标插入指示线
    float dropIndicatorY = 0.0f;     // 目标插入指示线在内容坐标系中的 Y 坐标（DIP）
};

// 顶栏与导航栏视口及热区常量（DIP，左右严格对齐 12.0f ~ 348.0f 视觉网格）
inline constexpr float kViewportTopDip           = Layout::TOP_BAR_HEIGHT + Layout::NAV_BAR_HEIGHT; // 76.0f
inline constexpr D2D1_RECT_F kCloseBtnRect       = { 320.0f, 6.0f, 348.0f, 34.0f };
inline constexpr D2D1_RECT_F kNavSegmentTrackRect= { 12.0f, 42.0f, 92.0f, 70.0f };
inline constexpr D2D1_RECT_F kTabHistoryRect     = { 14.0f, 44.0f, 51.0f, 68.0f };
inline constexpr D2D1_RECT_F kTabPinnedRect      = { 53.0f, 44.0f, 90.0f, 68.0f };
inline constexpr D2D1_RECT_F kClearAllBtnRect    = { 282.0f, 43.0f, 348.0f, 69.0f };

// 卡片右侧操作区按钮热区计算（DIP）：从上到下依次为 固定、粘贴为纯文本、删除
inline constexpr D2D1_RECT_F GetCardPinBtnRect(float cardTop) noexcept {
    constexpr float cardRight = Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ;
    constexpr float btnLeft = (cardRight - Layout::CARD_ACTION_WIDTH) + 2.0f;
    constexpr float btnRight = cardRight - 4.0f;
    return { btnLeft, cardTop + 4.5f, btnRight, cardTop + 25.5f };
}

inline constexpr D2D1_RECT_F GetCardPlainPasteBtnRect(float cardTop) noexcept {
    constexpr float cardRight = Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ;
    constexpr float btnLeft = (cardRight - Layout::CARD_ACTION_WIDTH) + 2.0f;
    constexpr float btnRight = cardRight - 4.0f;
    return { btnLeft, cardTop + 27.5f, btnRight, cardTop + 48.5f };
}

inline constexpr D2D1_RECT_F GetCardDeleteBtnRect(float cardTop, float cardHeight) noexcept {
    constexpr float cardRight = Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ;
    constexpr float btnLeft = (cardRight - Layout::CARD_ACTION_WIDTH) + 2.0f;
    constexpr float btnRight = cardRight - 4.0f;
    const float cardBottom = cardTop + cardHeight;
    return { btnLeft, cardBottom - 25.5f, btnRight, cardBottom - 4.5f };
}

constexpr bool IsImageClipItem(const ClipItem& item) noexcept {
    return item.type == ClipItemType::Image;
}

inline bool IsMixedPreviewClipItem(const ClipItem& item) noexcept {
    return item.type != ClipItemType::Image && !item.imageFileName.empty() && !item.previewText.empty();
}

inline bool HasClipPreviewImage(const ClipItem& item) noexcept {
    return IsImageClipItem(item) || IsMixedPreviewClipItem(item);
}

inline bool CanPlainTextPaste(const ClipItem& item) noexcept {
    return !IsImageClipItem(item) && (!item.previewText.empty() || !item.textFileName.empty());
}

inline float GetClipCardHeight(const ClipItem& item) noexcept {
    return HasClipPreviewImage(item) ? Layout::CARD_IMAGE_HEIGHT : Layout::CARD_TEXT_HEIGHT;
}

constexpr bool PointInRect(float x, float y, const D2D1_RECT_F& rc) noexcept {
    return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
}

struct ScrollbarMetrics {
    bool isVisible = false;
    float trackTop = 0.0f;
    float trackBottom = 0.0f;
    float trackHeight = 0.0f;
    float thumbTop = 0.0f;
    float thumbBottom = 0.0f;
    float thumbHeight = 0.0f;
    float scrollableTrackHeight = 0.0f;
    float maxScroll = 0.0f;
};

inline ScrollbarMetrics CalculateScrollbarMetrics(float totalContentHeight, float scrollOffsetY) noexcept {
    ScrollbarMetrics m{};
    if (totalContentHeight <= Layout::VIEWPORT_HEIGHT) {
        return m;
    }

    m.isVisible = true;
    m.trackTop = kViewportTopDip + 4.0f;
    m.trackBottom = Layout::WINDOW_HEIGHT - 4.0f;
    m.trackHeight = std::max(1.0f, m.trackBottom - m.trackTop);

    const float visibleRatio = std::clamp(Layout::VIEWPORT_HEIGHT / totalContentHeight, 0.08f, 1.0f);
    m.thumbHeight = std::max(24.0f, m.trackHeight * visibleRatio);
    m.scrollableTrackHeight = std::max(1.0f, m.trackHeight - m.thumbHeight);

    m.maxScroll = std::max(1.0f, totalContentHeight - Layout::VIEWPORT_HEIGHT);
    const float scrollRatio = std::clamp(scrollOffsetY / m.maxScroll, 0.0f, 1.0f);
    m.thumbTop = m.trackTop + m.scrollableTrackHeight * scrollRatio;
    m.thumbBottom = m.thumbTop + m.thumbHeight;
    return m;
}

class VirtualListLayout {
public:
    VirtualListLayout() = default;
    ~VirtualListLayout() = default;

    // 确保虚拟列表 O(1) 高度与 O(log N) 二分查找前缀和缓存与当前 items 同步
    void EnsureLayoutCache(const ClipItems& items) const;

    // 计算列表总内容高度（DIP），用于滚动边界钳制
    float CalculateTotalContentHeight(const ClipItems& items) const;

    // 二分查找定位第一张可能进入可视区的卡片索引（调用前需确保已调用 EnsureLayoutCache）
    size_t FindFirstVisibleCandidateIndex(float viewStart) const;

    // 获取指定索引卡片的顶部相对内容区 Y 坐标（DIP）
    float GetItemTopY(size_t index) const noexcept {
        return prefixTopY_[index] - layoutOrigin_;
    }

    // 获取已缓存的列表总内容高度（DIP）
    float GetCachedTotalHeight() const noexcept {
        return cachedTotalHeight_;
    }

    // 查询指定卡片在内容坐标系中的上下边界（DIP），用于键盘导航自动滚动到可视区
    bool GetItemVerticalBounds(
        uint64_t itemId,
        const ClipItems& items,
        float& outTopY,
        float& outBottomY) const;

    // 命中测试（传入坐标已转换为 DIP）
    HitTestResult HitTest(float dipX, float dipY, const ClipItems& items, const UIViewState& state) const;

    // 计算固定页拖动排序时的目标插入槽位与指示线坐标（传入 dipY 为窗口客户区 DIP 坐标）
    DropTargetSlot CalculateDropSlot(
        float dipY,
        uint64_t draggingItemId,
        const ClipItems& items,
        const UIViewState& state) const;

    // 显式失效布局缓存（如数据变动或拖动排序改变卡片顺序后调用）
    void InvalidateCache() const noexcept {
        cachedItemsPtr_ = nullptr;
        cachedItemCount_ = 0;
    }

private:
    // 虚拟列表 O(1) 高度与 O(log N) 二分查找缓存
    mutable std::deque<float> prefixTopY_;
    mutable float layoutOrigin_ = 0.0f;
    mutable float cachedTotalHeight_ = 0.0f;
    mutable const ClipItems* cachedItemsPtr_ = nullptr;
    mutable size_t cachedItemCount_ = 0;
    mutable uint64_t cachedFirstId_ = 0;
    mutable int64_t cachedFirstTimestamp_ = 0;
    mutable uint64_t cachedLastId_ = 0;
};
