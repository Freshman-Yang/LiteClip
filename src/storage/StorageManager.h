#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include <unordered_map>

class StorageManager {
public:
    StorageManager();
    ~StorageManager();

    bool Initialize();

    // 捕获新内容并落盘；deque 头部插入 O(1)，哈希索引筛选后验证完整内容。
    // 富文本同时验证 HTML、RTF 和本地资源，不能仅凭预览或候选哈希合并。
    // 若完整内容匹配 historyItems_ 第 1 条（最新一条），更新剪切板序列号并返回 false。
    // 若匹配后续已存在记录（无论是否固定）的去重特征，将其移至列表顶部并更新时间戳，无需重复落盘。
    // 否则在头部插入新记录。
    bool AddCapturedItem(const RawClipboardPayload& payload);

    const ClipItems& GetHistoryItems() const { return historyItems_; }
    const ClipItems& GetPinnedItems() const { return pinnedItems_; }
    const ClipItem* FindItemById(uint64_t id) const;

    // 固定/取消固定：
    // 固定项同时存在于 historyItems_ 和 pinnedItems_ 中。
    // 每次状态改变必须立即调用 SavePinnedIndex() 持久化到本地磁盘。
    bool SetItemPinned(uint64_t id, bool pinned);

    // 固定页拖动排序：
    // 仅调整 pinnedItems_ 中的顺序并立即持久化到本地磁盘，historyItems_ 保持时间线顺序不变。
    bool MovePinnedItem(uint64_t id, size_t targetIndex);

    // 删除单条记录：从两个列表中移除，若原本是固定项则立即更新持久化存储，并删除其关联的本地缓存文件。
    bool DeleteItem(uint64_t id);

    // 全部清除：
    // 若 tab == ActiveTab::History：仅清除 historyItems_ 中所有 isPinned == false 的项目并删除其磁盘文件，已固定的项目保留在原位。
    // 若 tab == ActiveTab::Pinned：清除所有固定项目（同时将其从 historyItems_、pinnedItems_ 和本地持久化存储中移除，并删除其磁盘文件）。
    bool ClearAll(ActiveTab tab);

    // 按需读取；纯文本粘贴不加载 HTML/RTF/图片，也不依赖这些文件存在。
    bool LoadItemPayload(uint64_t id, RawClipboardPayload& outPayload, bool plainTextOnly = false) const;

    // 获取图片或缓存文件的完整本地路径
    std::filesystem::path GetDataDir() const { return dataDir_; }
    std::wstring GetImageFullPath(const std::wstring& imageFileName) const;

private:
    std::filesystem::path baseDir_;
    std::filesystem::path dataDir_;
    std::filesystem::path pinnedIndexPath_;
    uint64_t nextId_ = 1;
    ClipItems historyItems_;
    ClipItems pinnedItems_;

    // O(1) 查找与去重索引：
    // historyIdToRevIdx_ 存储 (historyItems_.size() - 1 - index)，在头部插入新元素时无需重建旧元素索引
    std::unordered_map<uint64_t, size_t> historyIdToRevIdx_;
    std::unordered_map<uint64_t, size_t> pinnedIdToIdx_;
    std::unordered_map<uint64_t, std::vector<uint64_t>> hashToIds_;

    void RebuildHistoryIndex();
    void RebuildPinnedIndex();

    std::wstring MakePreviewText(const std::wstring& fullText) const;
    std::wstring GenerateScreenshotFileName() const;
    void DeleteItemDiskFiles(const ClipItem& item);
    bool SavePinnedIndex(const ClipItems& items) const;
    bool LoadPinnedIndex();
    void CleanupOrphanFiles() const;
};
