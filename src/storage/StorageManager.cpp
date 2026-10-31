#include "storage/StorageManager.h"
#include "clipboard/HtmlFormatProcessor.h"
#include "core/Hash.h"
#include "platform/FileUtils.h"
#include "platform/I18n.h"

#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace {

constexpr uint32_t kPinnedIndexMagic   = 0x4C43504E; // 'LCPN'
constexpr size_t   kMaxPreviewChars    = 200;
constexpr uint32_t kMaxScreenshotSeq   = 10000;

} // namespace

StorageManager::StorageManager() = default;

StorageManager::~StorageManager() = default;

void StorageManager::RebuildHistoryIndex() {
    historyIdToRevIdx_.clear();
    hashToIds_.clear();
    const size_t total = historyItems_.size();
    historyIdToRevIdx_.reserve(total);
    for (size_t i = 0; i < total; ++i) {
        const ClipItem& item = historyItems_[i];
        historyIdToRevIdx_[item.id] = total - 1 - i;
        hashToIds_[item.contentHash].push_back(item.id);
    }
}

void StorageManager::RebuildPinnedIndex() {
    pinnedIdToIdx_.clear();
    const size_t total = pinnedItems_.size();
    pinnedIdToIdx_.reserve(total);
    for (size_t i = 0; i < total; ++i) {
        pinnedIdToIdx_[pinnedItems_[i].id] = i;
    }
}

bool StorageManager::Initialize() {
    PWSTR localAppDataPath = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppDataPath);
    if (FAILED(hr) || localAppDataPath == nullptr) {
        return false;
    }

    baseDir_ = std::filesystem::path(localAppDataPath) / L"LiteClip";
    CoTaskMemFree(localAppDataPath);

    dataDir_ = baseDir_ / L"data";
    pinnedIndexPath_ = baseDir_ / L"pinned.dat";

    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    if (ec || !std::filesystem::exists(dataDir_, ec)) {
        return false;
    }

    historyItems_.clear();
    pinnedItems_.clear();
    historyIdToRevIdx_.clear();
    pinnedIdToIdx_.clear();
    hashToIds_.clear();
    nextId_ = 1;

    if (!LoadPinnedIndex()) {
        MessageBoxW(nullptr, I18n::Get().errLoadPinnedFailed, L"LiteClip", MB_OK | MB_ICONERROR);
        return false;
    }
    CleanupOrphanFiles();

    return true;
}

bool StorageManager::AddCapturedItem(const RawClipboardPayload& payload) {
    if (payload.type == ClipItemType::Image) {
        if (payload.pngBytes.empty()) {
            return false;
        }
    } else {
        if (payload.plainText.empty() && payload.htmlBytes.empty() && payload.rtfBytes.empty()) {
            return false;
        }
    }

    // 1. 构造预览文本：当无文字且无图（如空表格）时保持为空字符串 L""；图片项同样无需塞入文件名
    std::wstring candidatePreview;
    if (payload.type != ClipItemType::Image) {
        candidatePreview = MakePreviewText(payload.plainText);
    }

    // 2. 候选特征与完整内容验证
    const auto isSameContent = [this, &payload, &candidatePreview](const ClipItem& existing) -> bool {
        if (existing.contentHash != payload.contentHash || existing.type != payload.type) {
            return false;
        }
        // 格式组合必须一致（例如不能把只含 HTML 的中型项与含 HTML+RTF 的小型项混为一谈）
        if (existing.htmlFileName.empty() != payload.htmlBytes.empty() ||
            existing.rtfFileName.empty() != payload.rtfBytes.empty()) {
            return false;
        }
        if (payload.type == ClipItemType::Image) {
            if (existing.imageWidth != payload.imageWidth || existing.imageHeight != payload.imageHeight ||
                !FileEqualsBytes(dataDir_ / existing.imageFileName, payload.pngBytes.data(), payload.pngBytes.size())) {
                return false;
            }
        } else {
            // 先比对内存中的 200 字预览文本（0 磁盘开销）
            if (existing.previewText != candidatePreview) {
                return false;
            }
            // 仅当纯文本超过 2MB（触发了 ComputeFnv1aHash 的抽样哈希）时，才需要读盘校验完整纯文本；
            // 2MB 以内的纯文本和规范化后的 HTML Fragment 已经做过全字节精确 FNV-1a 哈希，无需再读盘！
            constexpr size_t kFullHashBytes = 2ULL * 1024ULL * 1024ULL;
            const size_t textBytes = payload.plainText.size() * sizeof(wchar_t);
            if (textBytes > kFullHashBytes &&
                !FileEqualsBytes(dataDir_ / existing.textFileName, payload.plainText.data(), textBytes)) {
                return false;
            }
        }
        return true;
    };

    // 辅助 Lambda：持久化保存 HTML 及其关联的本地高清图片资产目录
    const auto saveHtmlWithAssets = [&](ClipItem& targetItem) -> bool {
        if (payload.htmlBytes.empty()) {
            return true;
        }
        if (targetItem.htmlFileName.empty()) {
            targetItem.htmlFileName = L"html_" + std::to_wstring(targetItem.id) + L".bin";
        }
        const std::filesystem::path htmlPath = dataDir_ / targetItem.htmlFileName;

        if (!payload.htmlLocalAssetFiles.empty()) {
            targetItem.assetDirName = L"assets_" + std::to_wstring(targetItem.id);
            const std::filesystem::path assetDirPath = dataDir_ / targetItem.assetDirName;
            std::vector<uint8_t> rewrittenHtmlBytes = payload.htmlBytes;
            if (!HtmlFormatProcessor::PersistHtmlAssetsAndRewriteUris(
                rewrittenHtmlBytes,
                payload.htmlLocalAssetFiles,
                assetDirPath)) {
                return false;
            }
            return WriteBinaryFile(htmlPath, rewrittenHtmlBytes.data(), rewrittenHtmlBytes.size());
        }

        return WriteBinaryFile(htmlPath, payload.htmlBytes.data(), payload.htmlBytes.size());
    };

    if (!historyItems_.empty()) {
        if (isSameContent(historyItems_.front())) {
            // 即使与第一条内容相同，用户在原程序重新按了 Ctrl+C 也会使系统剪切板序列号递增，
            // 必须同步更新第一条的序列号，确保后续点击第一条时依然能触发直通粘贴！
            if (payload.clipboardSeqNum != 0) {
                historyItems_.front().clipboardSeqNum = payload.clipboardSeqNum;
                if (historyItems_.front().isPinned) {
                    auto pinIdxIt = pinnedIdToIdx_.find(historyItems_.front().id);
                    if (pinIdxIt != pinnedIdToIdx_.end() && pinIdxIt->second < pinnedItems_.size()) {
                        pinnedItems_[pinIdxIt->second].clipboardSeqNum = payload.clipboardSeqNum;
                    }
                }
            }
            return false;
        }
    }

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 3. 利用 hashToIds_ 进行 O(1) 哈希查重，避免在无上限历史中线性扫描整个 historyItems_
    auto hashIt = hashToIds_.find(payload.contentHash);
    if (hashIt != hashToIds_.end()) {
        const size_t total = historyItems_.size();
        for (uint64_t candidateId : hashIt->second) {
            auto revIt = historyIdToRevIdx_.find(candidateId);
            if (revIt == historyIdToRevIdx_.end() || revIt->second >= total) {
                continue;
            }
            const size_t idx = total - 1 - revIt->second;
            if (idx > 0 && isSameContent(historyItems_[idx])) {
                auto it = historyItems_.begin() + static_cast<std::ptrdiff_t>(idx);
                it->timestamp = nowMs;
                it->clipboardSeqNum = payload.clipboardSeqNum; // 同步更新为当前系统剪切板最新序列号
                if (it->isPinned) {
                    auto pinIdxIt = pinnedIdToIdx_.find(it->id);
                    if (pinIdxIt != pinnedIdToIdx_.end() && pinIdxIt->second < pinnedItems_.size()) {
                        pinnedItems_[pinIdxIt->second].timestamp = nowMs;
                        pinnedItems_[pinIdxIt->second].clipboardSeqNum = payload.clipboardSeqNum;
                    }
                }
                std::rotate(historyItems_.begin(), it, std::next(it));
                for (size_t i = 0; i <= idx; ++i) {
                    historyIdToRevIdx_[historyItems_[i].id] = total - 1 - i;
                }
                return true;
            }
        }
    }

    ClipItem item{};
    item.id = nextId_++;
    item.type = payload.type;
    item.contentHash = payload.contentHash;
    item.timestamp = nowMs;
    item.clipboardSeqNum = payload.clipboardSeqNum;
    item.isPinned = false;
    item.imageWidth = payload.imageWidth;
    item.imageHeight = payload.imageHeight;

    if (payload.type == ClipItemType::Image) {
        item.imageFileName = GenerateScreenshotFileName();
        if (item.imageFileName.empty()) {
            return false;
        }

        const std::filesystem::path imgPath = dataDir_ / item.imageFileName;
        if (!WriteBinaryFile(imgPath, payload.pngBytes.data(), payload.pngBytes.size())) {
            DeleteItemDiskFiles(item);
            return false;
        }

        // 若是从无字富文本中提取的缩略图，同时保存其富文本数据及关联高清原图资产以便粘贴 100% 还原
        if (!saveHtmlWithAssets(item)) {
            DeleteItemDiskFiles(item);
            return false;
        }

        if (!payload.rtfBytes.empty()) {
            item.rtfFileName = L"rtf_" + std::to_wstring(item.id) + L".bin";
            const std::filesystem::path rtfPath = dataDir_ / item.rtfFileName;
            if (!WriteBinaryFile(rtfPath, payload.rtfBytes.data(), payload.rtfBytes.size())) {
                DeleteItemDiskFiles(item);
                return false;
            }
        }

        item.previewText.clear();
    } else {
        item.textFileName = L"txt_" + std::to_wstring(item.id) + L".bin";
        const std::filesystem::path txtPath = dataDir_ / item.textFileName;
        if (!WriteBinaryFile(txtPath, payload.plainText.data(), payload.plainText.size() * sizeof(wchar_t))) {
            DeleteItemDiskFiles(item);
            return false;
        }

        if (payload.type == ClipItemType::RichText || !payload.htmlBytes.empty() || !payload.rtfBytes.empty()) {
            if (!saveHtmlWithAssets(item)) {
                DeleteItemDiskFiles(item);
                return false;
            }

            if (!payload.rtfBytes.empty()) {
                item.rtfFileName = L"rtf_" + std::to_wstring(item.id) + L".bin";
                const std::filesystem::path rtfPath = dataDir_ / item.rtfFileName;
                if (!WriteBinaryFile(rtfPath, payload.rtfBytes.data(), payload.rtfBytes.size())) {
                    DeleteItemDiskFiles(item);
                    return false;
                }
            }
        }

        item.previewText = std::move(candidatePreview);

        // 若文字不够填满 1 倍高度且附带提取出的首图缩略图，持久化保存缩略图供 UI 按 2 倍高度“左文右图”展示
        if (!payload.pngBytes.empty() && !item.previewText.empty() && !DoesTextFillSingleCard(item.previewText)) {
            std::wstring thumbName = L"thumb_" + std::to_wstring(item.id) + L".png";
            if (WriteBinaryFile(dataDir_ / thumbName, payload.pngBytes.data(), payload.pngBytes.size())) {
                item.imageFileName = std::move(thumbName);
            } else {
                item.imageWidth = 0;
                item.imageHeight = 0;
            }
        } else {
            item.imageWidth = 0;
            item.imageHeight = 0;
        }
    }

    const uint64_t newId = item.id;
    const uint64_t newHash = item.contentHash;
    historyItems_.insert(historyItems_.begin(), std::move(item));

    // O(1) 增量更新倒序索引与哈希桶
    historyIdToRevIdx_[newId] = historyItems_.size() - 1;
    hashToIds_[newHash].push_back(newId);

    return true;
}

const ClipItem* StorageManager::FindItemById(uint64_t id) const {
    auto revIt = historyIdToRevIdx_.find(id);
    if (revIt != historyIdToRevIdx_.end() && revIt->second < historyItems_.size()) {
        const size_t idx = historyItems_.size() - 1 - revIt->second;
        return &historyItems_[idx];
    }

    auto pinIt = pinnedIdToIdx_.find(id);
    if (pinIt != pinnedIdToIdx_.end() && pinIt->second < pinnedItems_.size()) {
        return &pinnedItems_[pinIt->second];
    }

    return nullptr;
}

bool StorageManager::SetItemPinned(uint64_t id, bool pinned) {
    ClipItem* histPtr = nullptr;
    auto revIt = historyIdToRevIdx_.find(id);
    if (revIt != historyIdToRevIdx_.end() && revIt->second < historyItems_.size()) {
        histPtr = &historyItems_[historyItems_.size() - 1 - revIt->second];
    }

    ClipItem* pinPtr = nullptr;
    size_t pinIdx = 0;
    auto pinIt = pinnedIdToIdx_.find(id);
    if (pinIt != pinnedIdToIdx_.end() && pinIt->second < pinnedItems_.size()) {
        pinIdx = pinIt->second;
        pinPtr = &pinnedItems_[pinIdx];
    }

    if (!histPtr && !pinPtr) {
        return false;
    }

    auto newPinnedItems = pinnedItems_;
    if (pinned && !pinPtr && histPtr) {
        ClipItem item = *histPtr;
        item.isPinned = true;
        newPinnedItems.insert(newPinnedItems.begin(), std::move(item));
    } else if (!pinned && pinPtr) {
        newPinnedItems.erase(newPinnedItems.begin() + static_cast<std::ptrdiff_t>(pinIdx));
    }
    if (!SavePinnedIndex(newPinnedItems)) return false;

    if (pinned) {
        if (histPtr) {
            histPtr->isPinned = true;
        } else if (pinPtr) {
            pinPtr->isPinned = true;
            historyItems_.insert(historyItems_.begin(), *pinPtr);
            historyIdToRevIdx_[pinPtr->id] = historyItems_.size() - 1;
            hashToIds_[pinPtr->contentHash].push_back(pinPtr->id);
        }
    } else {
        if (histPtr) {
            histPtr->isPinned = false;
        }
    }

    pinnedItems_.swap(newPinnedItems);
    RebuildPinnedIndex();
    return true;
}

bool StorageManager::MovePinnedItem(uint64_t id, size_t targetIndex) {
    auto it = pinnedIdToIdx_.find(id);
    if (it == pinnedIdToIdx_.end() || pinnedItems_.empty()) {
        return false;
    }

    const size_t fromIndex = it->second;
    if (fromIndex >= pinnedItems_.size() || pinnedItems_[fromIndex].id != id) {
        return false;
    }

    const size_t toIndex = std::min(targetIndex, pinnedItems_.size() - 1);
    if (fromIndex == toIndex) {
        return true;
    }

    auto newPinnedItems = pinnedItems_;
    if (fromIndex < toIndex) {
        std::rotate(
            newPinnedItems.begin() + static_cast<std::ptrdiff_t>(fromIndex),
            newPinnedItems.begin() + static_cast<std::ptrdiff_t>(fromIndex + 1),
            newPinnedItems.begin() + static_cast<std::ptrdiff_t>(toIndex + 1));
    } else {
        std::rotate(
            newPinnedItems.begin() + static_cast<std::ptrdiff_t>(toIndex),
            newPinnedItems.begin() + static_cast<std::ptrdiff_t>(fromIndex),
            newPinnedItems.begin() + static_cast<std::ptrdiff_t>(fromIndex + 1));
    }

    if (!SavePinnedIndex(newPinnedItems)) {
        return false;
    }

    pinnedItems_.swap(newPinnedItems);
    RebuildPinnedIndex();
    return true;
}

bool StorageManager::DeleteItem(uint64_t id) {
    auto newPinnedItems = pinnedItems_;
    const bool removesPinned = std::erase_if(newPinnedItems, [id](const ClipItem& item) {
        return item.id == id;
    }) != 0;
    if (removesPinned && !SavePinnedIndex(newPinnedItems)) return false;

    ClipItem targetItem{};
    bool found = false;

    auto revIt = historyIdToRevIdx_.find(id);
    if (revIt != historyIdToRevIdx_.end() && revIt->second < historyItems_.size()) {
        const size_t idx = historyItems_.size() - 1 - revIt->second;
        targetItem = historyItems_[idx];
        found = true;
        historyItems_.erase(historyItems_.begin() + static_cast<std::ptrdiff_t>(idx));
        RebuildHistoryIndex();
    }

    auto pinIt = pinnedIdToIdx_.find(id);
    if (pinIt != pinnedIdToIdx_.end() && pinIt->second < pinnedItems_.size()) {
        const size_t pinIdx = pinIt->second;
        if (!found) {
            targetItem = pinnedItems_[pinIdx];
            found = true;
        }
        pinnedItems_.erase(pinnedItems_.begin() + static_cast<std::ptrdiff_t>(pinIdx));
        RebuildPinnedIndex();
    }

    if (!found) {
        return false;
    }

    DeleteItemDiskFiles(targetItem);

    return true;
}

bool StorageManager::ClearAll(ActiveTab tab) {
    if (tab == ActiveTab::Pinned && !SavePinnedIndex({})) return false;
    if (tab == ActiveTab::History) {
        for (const auto& item : historyItems_) {
            if (!item.isPinned) {
                DeleteItemDiskFiles(item);
            }
        }
        std::erase_if(historyItems_, [](const ClipItem& item) {
            return !item.isPinned;
        });
        RebuildHistoryIndex();
    } else {
        std::unordered_set<uint64_t> deletedIds;
        deletedIds.reserve(pinnedItems_.size());
        for (const auto& item : pinnedItems_) {
            DeleteItemDiskFiles(item);
            deletedIds.insert(item.id);
        }
        pinnedItems_.clear();
        pinnedIdToIdx_.clear();

        for (const auto& item : historyItems_) {
            if (item.isPinned && !deletedIds.contains(item.id)) {
                DeleteItemDiskFiles(item);
            }
        }
        std::erase_if(historyItems_, [&deletedIds](const ClipItem& item) {
            return item.isPinned || deletedIds.contains(item.id);
        });
        RebuildHistoryIndex();

    }
    return true;
}

bool StorageManager::LoadItemPayload(uint64_t id, RawClipboardPayload& outPayload, bool plainTextOnly) const {
    const ClipItem* item = FindItemById(id);
    if (item == nullptr) {
        return false;
    }

    outPayload = RawClipboardPayload{};
    outPayload.type = item->type;
    outPayload.contentHash = item->contentHash;
    outPayload.clipboardSeqNum = item->clipboardSeqNum;
    outPayload.imageWidth = item->imageWidth;
    outPayload.imageHeight = item->imageHeight;

    if (plainTextOnly) {
        if (item->type == ClipItemType::Image || !IsSafeFileName(item->textFileName)) return false;
        outPayload.type = ClipItemType::PlainText;
        return ReadUtf16File(dataDir_ / item->textFileName, outPayload.plainText);
    }

    if (item->type == ClipItemType::Image) {
        if (!IsSafeFileName(item->imageFileName)) {
            return false;
        }
        const std::filesystem::path imgPath = dataDir_ / item->imageFileName;
        if (!ReadBinaryFile(imgPath, outPayload.pngBytes) || outPayload.pngBytes.empty()) {
            return false;
        }
        if (!item->htmlFileName.empty() && IsSafeFileName(item->htmlFileName)) {
            ReadBinaryFile(dataDir_ / item->htmlFileName, outPayload.htmlBytes);
        }
        if (!item->rtfFileName.empty() && IsSafeFileName(item->rtfFileName)) {
            ReadBinaryFile(dataDir_ / item->rtfFileName, outPayload.rtfBytes);
        }
        return true;
    }

    bool anyLoaded = false;

    if (!item->textFileName.empty()) {
        if (!IsSafeFileName(item->textFileName)) {
            return false;
        }
        const std::filesystem::path txtPath = dataDir_ / item->textFileName;
        if (!ReadUtf16File(txtPath, outPayload.plainText)) {
            return false;
        }
        anyLoaded = true;
    }

    if (!item->htmlFileName.empty()) {
        if (!IsSafeFileName(item->htmlFileName)) {
            return false;
        }
        const std::filesystem::path htmlPath = dataDir_ / item->htmlFileName;
        if (!ReadBinaryFile(htmlPath, outPayload.htmlBytes)) {
            return false;
        }
        anyLoaded = true;
    }

    if (!item->rtfFileName.empty()) {
        if (!IsSafeFileName(item->rtfFileName)) {
            return false;
        }
        const std::filesystem::path rtfPath = dataDir_ / item->rtfFileName;
        if (!ReadBinaryFile(rtfPath, outPayload.rtfBytes)) {
            return false;
        }
        anyLoaded = true;
    }

    return anyLoaded;
}

std::wstring StorageManager::GetImageFullPath(const std::wstring& imageFileName) const {
    if (!IsSafeFileName(imageFileName)) {
        return L"";
    }
    return (dataDir_ / imageFileName).wstring();
}

std::wstring StorageManager::MakePreviewText(const std::wstring& fullText) const {
    std::wstring result;
    result.reserve(kMaxPreviewChars);

    const size_t len = fullText.size();
    size_t i = 0;

    // 跳过开头的空白字符
    while (i < len && std::iswspace(fullText[i])) {
        ++i;
    }

    bool pendingNewline = false;
    bool pendingSpace = false;

    for (; i < len && result.size() < kMaxPreviewChars; ++i) {
        const wchar_t ch = fullText[i];

        if (ch == L'\r' || ch == L'\n') {
            if (ch == L'\r' && (i + 1 < len) && fullText[i + 1] == L'\n') {
                ++i;
            }
            pendingNewline = true;
            pendingSpace = false;
        } else if (std::iswspace(ch)) {
            if (!pendingNewline) {
                pendingSpace = true;
            }
        } else {
            if (pendingNewline) {
                if (!result.empty() && result.size() < kMaxPreviewChars) {
                    result.push_back(L'\n');
                }
                pendingNewline = false;
                pendingSpace = false;
            } else if (pendingSpace) {
                if (!result.empty() && result.size() < kMaxPreviewChars) {
                    result.push_back(L' ');
                }
                pendingSpace = false;
            }

            if (result.size() < kMaxPreviewChars) {
                result.push_back(ch);
            }
        }
    }

    // 若截断处恰好落在一个 UTF-16 代理对的高位代理（0xD800..0xDBFF）上，
    // 移除末尾孤立的高位代理字符，防止 D2D 渲染乱码框及持久化索引污染。
    if (!result.empty() && IsHighSurrogate(result.back())) {
        result.pop_back();
    }

    return result;
}

std::wstring StorageManager::GenerateScreenshotFileName() const {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t baseName[64]{};
    swprintf_s(baseName, L"%ls%04u%02u%02u_%02u%02u%02u",
               I18n::Get().screenshotPrefix,
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    std::error_code ec;
    std::wstring candidate = std::wstring(baseName) + L".png";
    if (!std::filesystem::exists(dataDir_ / candidate, ec)) {
        return candidate;
    }

    // 限制同一秒内最多追加序号至 10000，避免极端文件系统异常下执行海量磁盘查询导致主线程卡死
    for (uint32_t seq = 1; seq <= kMaxScreenshotSeq; ++seq) {
        candidate = std::wstring(baseName) + L"_" + std::to_wstring(seq) + L".png";
        if (!std::filesystem::exists(dataDir_ / candidate, ec)) {
            return candidate;
        }
    }

    return L"";
}

void StorageManager::DeleteItemDiskFiles(const ClipItem& item) {
    std::error_code ec;
    const auto removeIfSafe = [this, &ec](const std::wstring& fileName) {
        if (!fileName.empty() && IsSafeFileName(fileName)) {
            std::filesystem::remove(dataDir_ / fileName, ec);
        }
    };

    removeIfSafe(item.textFileName);
    removeIfSafe(item.htmlFileName);
    removeIfSafe(item.rtfFileName);
    removeIfSafe(item.imageFileName);

    if (!item.assetDirName.empty() && IsSafeFileName(item.assetDirName)) {
        std::filesystem::remove_all(dataDir_ / item.assetDirName, ec);
    }
}

bool StorageManager::SavePinnedIndex(const ClipItems& items) const {
    if (pinnedIndexPath_.empty()) {
        return false;
    }

    std::filesystem::path tmpPath = pinnedIndexPath_;
    tmpPath += L".tmp";

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }

        constexpr uint32_t kCurrentVersion = 2;
        const uint32_t count = static_cast<uint32_t>(items.size());

        if (!WritePod(ofs, kPinnedIndexMagic) ||
            !WritePod(ofs, kCurrentVersion) ||
            !WritePod(ofs, count)) {
            return false;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const ClipItem& item = items[i];
            const uint8_t typeVal = static_cast<uint8_t>(item.type);

            if (!WritePod(ofs, item.id) ||
                !WritePod(ofs, typeVal) ||
                !WritePod(ofs, item.contentHash) ||
                !WritePod(ofs, item.timestamp) ||
                !WritePod(ofs, item.imageWidth) ||
                !WritePod(ofs, item.imageHeight) ||
                !WriteWString(ofs, item.previewText) ||
                !WriteWString(ofs, item.textFileName) ||
                !WriteWString(ofs, item.htmlFileName) ||
                !WriteWString(ofs, item.rtfFileName) ||
                !WriteWString(ofs, item.imageFileName) ||
                !WriteWString(ofs, item.assetDirName)) {
                return false;
            }
        }

        ofs.flush();
        if (!ofs.good()) {
            return false;
        }
    }

    if (!MoveFileExW(tmpPath.c_str(), pinnedIndexPath_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    return true;
}

bool StorageManager::LoadPinnedIndex() {
    std::error_code ec;
    const bool exists = std::filesystem::exists(pinnedIndexPath_, ec);
    if (ec) return false;
    if (!exists) return true;
    if (!std::filesystem::is_regular_file(pinnedIndexPath_, ec) || ec) return false;

    std::ifstream ifs(pinnedIndexPath_, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;

    if (!ReadPod(ifs, magic) || magic != kPinnedIndexMagic) {
        return false;
    }
    if (!ReadPod(ifs, version) || (version != 1 && version != 2)) {
        return false;
    }
    if (!ReadPod(ifs, count)) {
        return false;
    }

    uint64_t maxId = 0;
    std::unordered_set<uint64_t> loadedIds;
    ClipItems loadedItems;

    for (uint32_t i = 0; i < count; ++i) {
        ClipItem item{};
        uint8_t typeVal = 0;

        if (!ReadPod(ifs, item.id) ||
            !ReadPod(ifs, typeVal) ||
            !ReadPod(ifs, item.contentHash) ||
            !ReadPod(ifs, item.timestamp) ||
            !ReadPod(ifs, item.imageWidth) ||
            !ReadPod(ifs, item.imageHeight) ||
            !ReadWString(ifs, item.previewText) ||
            !ReadWString(ifs, item.textFileName) ||
            !ReadWString(ifs, item.htmlFileName) ||
            !ReadWString(ifs, item.rtfFileName) ||
            !ReadWString(ifs, item.imageFileName)) {
            return false;
        }

        if (version >= 2) {
            if (!ReadWString(ifs, item.assetDirName)) {
                return false;
            }
        }

        if (typeVal > static_cast<uint8_t>(ClipItemType::Image) || item.id == 0 ||
            item.id == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        if (loadedIds.contains(item.id)) {
            return false;
        }

        item.type = static_cast<ClipItemType>(typeVal);
        item.isPinned = true;

        const auto checkFileValid = [this, &ec](const std::wstring& name) -> bool {
            if (name.empty()) {
                return true;
            }
            if (!IsSafeFileName(name)) {
                return false;
            }
            const std::filesystem::path fullPath = dataDir_ / name;
            return std::filesystem::exists(fullPath, ec) &&
                   std::filesystem::is_regular_file(fullPath, ec);
        };

        if (!checkFileValid(item.textFileName) ||
            !checkFileValid(item.htmlFileName) ||
            !checkFileValid(item.rtfFileName) ||
            !checkFileValid(item.imageFileName)) {
            return false;
        }

        if (!item.assetDirName.empty() && !IsSafeFileName(item.assetDirName)) {
            return false;
        }

        if (item.type == ClipItemType::Image) {
            if (item.imageFileName.empty()) {
                return false;
            }
            item.previewText.clear();
        } else {
            if (item.textFileName.empty() && item.htmlFileName.empty() && item.rtfFileName.empty()) {
                return false;
            }
        }

        maxId = std::max(maxId, item.id);
        loadedIds.insert(item.id);
        loadedItems.push_back(std::move(item));
    }

    pinnedItems_ = loadedItems;
    historyItems_ = std::move(loadedItems);
    std::stable_sort(historyItems_.begin(), historyItems_.end(), [](const ClipItem& a, const ClipItem& b) {
        if (a.timestamp != b.timestamp) {
            return a.timestamp > b.timestamp;
        }
        return a.id > b.id;
    });
    RebuildPinnedIndex();
    RebuildHistoryIndex();
    nextId_ = maxId + 1;
    return true;
}

void StorageManager::CleanupOrphanFiles() const {
    std::error_code ec;
    if (!std::filesystem::exists(dataDir_, ec)) {
        return;
    }

    std::unordered_set<std::wstring> activeNames;
    const auto collectFiles = [&activeNames](const ClipItems& items) {
        for (const auto& item : items) {
            if (!item.textFileName.empty())  activeNames.insert(item.textFileName);
            if (!item.htmlFileName.empty())  activeNames.insert(item.htmlFileName);
            if (!item.rtfFileName.empty())   activeNames.insert(item.rtfFileName);
            if (!item.imageFileName.empty()) activeNames.insert(item.imageFileName);
            if (!item.assetDirName.empty())  activeNames.insert(item.assetDirName);
        }
    };

    collectFiles(pinnedItems_);
    collectFiles(historyItems_);

    for (const auto& entry : std::filesystem::directory_iterator(dataDir_, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryEc;
        const std::wstring entryName = entry.path().filename().wstring();
        if (!activeNames.contains(entryName)) {
            if (entry.is_directory(entryEc)) {
                std::filesystem::remove_all(entry.path(), entryEc);
            } else if (entry.is_regular_file(entryEc)) {
                std::filesystem::remove(entry.path(), entryEc);
            }
        }
    }

    std::filesystem::path tmpIndex = pinnedIndexPath_;
    tmpIndex += L".tmp";
    if (std::filesystem::exists(tmpIndex, ec)) {
        std::filesystem::remove(tmpIndex, ec);
    }
}
