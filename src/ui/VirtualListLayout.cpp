#include "ui/VirtualListLayout.h"

#include <algorithm>
#include <iterator>

void VirtualListLayout::EnsureLayoutCache(const ClipItems& items) const {
    if (items.empty()) {
        prefixTopY_.clear();
        cachedTotalHeight_ = 0.0f;
        cachedItemsPtr_ = &items;
        cachedItemCount_ = 0;
        cachedFirstId_ = 0;
        cachedFirstTimestamp_ = 0;
        cachedLastId_ = 0;
        layoutOrigin_ = 0.0f;
        return;
    }

    const uint64_t firstId = items.front().id;
    const int64_t firstTs = items.front().timestamp;
    const uint64_t lastId = items.back().id;

    // 常见的头部新增只追加一个前缀坐标，旧坐标不移动。
    if (cachedItemsPtr_ == &items &&
        cachedItemCount_ > 0 && items.size() == cachedItemCount_ + 1 &&
        items[1].id == cachedFirstId_ && lastId == cachedLastId_ &&
        prefixTopY_.size() == cachedItemCount_) {
        const float addedHeight = GetClipCardHeight(items.front()) + Layout::CARD_GAP;
        prefixTopY_.push_front(prefixTopY_.front() - addedHeight);
        layoutOrigin_ = prefixTopY_.front() - Layout::LIST_PAD_TOP;
        cachedTotalHeight_ += addedHeight;
        cachedItemCount_ = items.size();
        cachedFirstId_ = firstId;
        cachedFirstTimestamp_ = firstTs;
        return;
    }

    if (cachedItemsPtr_ == &items &&
        cachedItemCount_ == items.size() &&
        cachedFirstId_ == firstId &&
        cachedFirstTimestamp_ == firstTs &&
        cachedLastId_ == lastId &&
        prefixTopY_.size() == items.size()) {
        return;
    }

    prefixTopY_.resize(items.size());
    layoutOrigin_ = 0.0f;
    float currentTopY = Layout::LIST_PAD_TOP;
    for (size_t i = 0; i < items.size(); ++i) {
        prefixTopY_[i] = currentTopY;
        currentTopY += GetClipCardHeight(items[i]);
        if (i + 1 < items.size()) {
            currentTopY += Layout::CARD_GAP;
        }
    }

    cachedTotalHeight_ = currentTopY + Layout::LIST_PAD_BOTTOM;
    cachedItemsPtr_ = &items;
    cachedItemCount_ = items.size();
    cachedFirstId_ = firstId;
    cachedFirstTimestamp_ = firstTs;
    cachedLastId_ = lastId;
}

float VirtualListLayout::CalculateTotalContentHeight(const ClipItems& items) const {
    EnsureLayoutCache(items);
    return cachedTotalHeight_;
}

bool VirtualListLayout::GetItemVerticalBounds(
    uint64_t itemId,
    const ClipItems& items,
    float& outTopY,
    float& outBottomY) const {
    if (itemId == 0 || items.empty()) {
        return false;
    }
    EnsureLayoutCache(items);
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == itemId) {
            outTopY = GetItemTopY(i);
            outBottomY = outTopY + GetClipCardHeight(items[i]);
            return true;
        }
    }
    return false;
}

size_t VirtualListLayout::FindFirstVisibleCandidateIndex(float viewStart) const {
    // 二分查找定位第一张可能进入可视区的卡片（卡片最大高度为 CARD_IMAGE_HEIGHT）
    const float minPossibleTopY = viewStart - Layout::CARD_IMAGE_HEIGHT;
    auto it = std::lower_bound(prefixTopY_.begin(), prefixTopY_.end(), minPossibleTopY + layoutOrigin_);
    return static_cast<size_t>(std::distance(prefixTopY_.begin(), it));
}

HitTestResult VirtualListLayout::HitTest(
    float dipX,
    float dipY,
    const ClipItems& items,
    const UIViewState& state) const {
    HitTestResult result{};
    if (dipX < 0.0f || dipX > Layout::WINDOW_WIDTH || dipY < 0.0f || dipY > Layout::WINDOW_HEIGHT) {
        return result;
    }

    // 1. 第一行：顶部栏 (0.0f ~ 40.0f DIP)
    if (dipY < Layout::TOP_BAR_HEIGHT) {
        if (PointInRect(dipX, dipY, kCloseBtnRect)) {
            result.region = HitRegionType::CloseButton;
            return result;
        }
        result.region = HitRegionType::TopBarDragArea;
        return result;
    }

    // 2. 第二行：导航与操作栏 (40.0f ~ 76.0f DIP)
    if (dipY < kViewportTopDip) {
        if (PointInRect(dipX, dipY, kTabHistoryRect)) {
            result.region = HitRegionType::TabHistory;
            return result;
        }
        if (PointInRect(dipX, dipY, kTabPinnedRect)) {
            result.region = HitRegionType::TabPinned;
            return result;
        }
        if (!items.empty() && PointInRect(dipX, dipY, kClearAllBtnRect)) {
            result.region = HitRegionType::ClearAllButton;
            return result;
        }
        return result;
    }

    // 3. 列表展示区 (76.0f ~ 420.0f DIP)
    if (items.empty()) {
        return result;
    }

    EnsureLayoutCache(items);

    if (cachedTotalHeight_ > Layout::VIEWPORT_HEIGHT &&
        dipX >= Layout::WINDOW_WIDTH - 10.0f) {
        return HitTestResult{ HitRegionType::Scrollbar, 0 };
    }

    const float cardLeft = Layout::LIST_PAD_HORZ;
    const float cardRight = Layout::WINDOW_WIDTH - Layout::LIST_PAD_HORZ;
    if (dipX < cardLeft || dipX > cardRight) {
        return result;
    }

    const float viewStart = std::max(0.0f, state.scrollOffsetY);
    const float contentY = (dipY - kViewportTopDip) + viewStart;

    // 二分查找定位第一个 itemTopY > contentY 的位置，其前一项即为唯一可能命中的卡片
    auto it = std::upper_bound(prefixTopY_.begin(), prefixTopY_.end(), contentY + layoutOrigin_);
    if (it == prefixTopY_.begin()) {
        return result;
    }

    const size_t idx = static_cast<size_t>(std::distance(prefixTopY_.begin(), it) - 1);
    const ClipItem& item = items[idx];
    const float itemTopY = prefixTopY_[idx] - layoutOrigin_;
    const float cardHeight = GetClipCardHeight(item);
    const float itemBottomY = itemTopY + cardHeight;

    if (contentY >= itemTopY && contentY <= itemBottomY) {
        result.itemId = item.id;
        const float localY = contentY - itemTopY;

        // 点击在卡片右侧操作区 (32.0f DIP 宽)：右上固定、右中粘贴为纯文本、右下删除
        const float actionAreaLeft = cardRight - Layout::CARD_ACTION_WIDTH;
        if (dipX >= actionAreaLeft && dipX < cardRight) {
            if (PointInRect(dipX, localY, GetCardPinBtnRect(0.0f))) {
                result.region = HitRegionType::CardPinButton;
                return result;
            }
            if (CanPlainTextPaste(item) && PointInRect(dipX, localY, GetCardPlainPasteBtnRect(0.0f))) {
                result.region = HitRegionType::CardPlainPasteButton;
                return result;
            }
            if (PointInRect(dipX, localY, GetCardDeleteBtnRect(0.0f, cardHeight))) {
                result.region = HitRegionType::CardDeleteButton;
                return result;
            }
        }

        result.region = HitRegionType::CardBody;
    }

    return result;
}

DropTargetSlot VirtualListLayout::CalculateDropSlot(
    float dipY,
    uint64_t draggingItemId,
    const ClipItems& items,
    const UIViewState& state) const {
    DropTargetSlot result{};
    const size_t n = items.size();
    if (n < 2 || draggingItemId == 0) {
        return result;
    }

    EnsureLayoutCache(items);

    size_t fromIndex = n;
    for (size_t i = 0; i < n; ++i) {
        if (items[i].id == draggingItemId) {
            fromIndex = i;
            break;
        }
    }
    if (fromIndex >= n) {
        return result;
    }

    const float viewStart = std::max(0.0f, state.scrollOffsetY);
    const float clampedDipY = std::clamp(dipY, kViewportTopDip, Layout::WINDOW_HEIGHT);
    const float contentY = (clampedDipY - kViewportTopDip) + viewStart;

    size_t slotIndex = 0;
    auto it = std::upper_bound(prefixTopY_.begin(), prefixTopY_.end(), contentY + layoutOrigin_);
    if (it == prefixTopY_.begin()) {
        slotIndex = 0;
    } else {
        const size_t idx = static_cast<size_t>(std::distance(prefixTopY_.begin(), it) - 1);
        const float itemTopY = GetItemTopY(idx);
        const float itemMidY = itemTopY + GetClipCardHeight(items[idx]) * 0.5f;
        slotIndex = (contentY < itemMidY) ? idx : (idx + 1);
    }

    const size_t targetItemIndex = (slotIndex <= fromIndex) ? slotIndex : (slotIndex - 1);
    const bool isEffectiveMove = (targetItemIndex != fromIndex);

    float indicatorContentY = 0.0f;
    if (slotIndex == 0) {
        indicatorContentY = GetItemTopY(0) - Layout::CARD_GAP * 0.5f;
    } else if (slotIndex < n) {
        indicatorContentY = GetItemTopY(slotIndex) - Layout::CARD_GAP * 0.5f;
    } else {
        const float lastTop = GetItemTopY(n - 1);
        const float lastHeight = GetClipCardHeight(items[n - 1]);
        indicatorContentY = lastTop + lastHeight + Layout::CARD_GAP * 0.5f;
    }

    result.slotIndex = slotIndex;
    result.targetItemIndex = targetItemIndex;
    result.indicatorContentY = indicatorContentY;
    result.isEffectiveMove = isEffectiveMove;
    return result;
}

