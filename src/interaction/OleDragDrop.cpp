#include "interaction/OleDragDrop.h"
#include "platform/GlobalMemory.h"

#include <ole2.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace {

bool g_isOverInternalTarget = false;

// ============================================================================
// 标准 IDropSource 实现：ClipDropSource
// ============================================================================
class ClipDropSource final : public IDropSource {
public:
    ClipDropSource() noexcept = default;
    ClipDropSource(const ClipDropSource&) = delete;
    ClipDropSource& operator=(const ClipDropSource&) = delete;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropSource)) {
            *ppvObject = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ::InterlockedIncrement(&refCount_);
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG count = ::InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IDropSource
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        if ((grfKeyState & MK_LBUTTON) == 0) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD /*dwEffect*/) override {
        if (g_isOverInternalTarget) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
            return S_OK;
        }
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~ClipDropSource() = default;
    volatile LONG refCount_ = 1;
};

// ============================================================================
// 窗口内部排序 IDropTarget 实现：InternalWindowDropTarget
// ============================================================================
class InternalWindowDropTarget final : public IDropTarget {
public:
    explicit InternalWindowDropTarget(InternalDropTargetCallbacks callbacks)
        : callbacks_(std::move(callbacks)) {}

    InternalWindowDropTarget(const InternalWindowDropTarget&) = delete;
    InternalWindowDropTarget& operator=(const InternalWindowDropTarget&) = delete;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget)) {
            *ppvObject = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ::InterlockedIncrement(&refCount_);
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG count = ::InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    STDMETHODIMP DragEnter(IDataObject* /*pDataObj*/, DWORD /*grfKeyState*/, POINTL pt, DWORD* pdwEffect) override {
        const POINT screenPt{ pt.x, pt.y };
        const bool accepted = callbacks_.onDragEnter && callbacks_.onDragEnter(screenPt);
        g_isOverInternalTarget = accepted;
        if (pdwEffect) {
            *pdwEffect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD /*grfKeyState*/, POINTL pt, DWORD* pdwEffect) override {
        const POINT screenPt{ pt.x, pt.y };
        const bool accepted = callbacks_.onDragOver && callbacks_.onDragOver(screenPt);
        g_isOverInternalTarget = accepted;
        if (pdwEffect) {
            *pdwEffect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragLeave() override {
        g_isOverInternalTarget = false;
        if (callbacks_.onDragLeave) {
            callbacks_.onDragLeave();
        }
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* /*pDataObj*/, DWORD /*grfKeyState*/, POINTL pt, DWORD* pdwEffect) override {
        g_isOverInternalTarget = false;
        const POINT screenPt{ pt.x, pt.y };
        const bool accepted = callbacks_.onDrop && callbacks_.onDrop(screenPt);
        if (pdwEffect) {
            *pdwEffect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

private:
    ~InternalWindowDropTarget() = default;
    volatile LONG refCount_ = 1;
    InternalDropTargetCallbacks callbacks_;
};

// ============================================================================
// 标准 IDataObject 实现：ClipDataObject
// ============================================================================
class ClipDataObject final : public IDataObject {
public:
    ClipDataObject() noexcept = default;
    ClipDataObject(const ClipDataObject&) = delete;
    ClipDataObject& operator=(const ClipDataObject&) = delete;

    // 注册挂载一个 HGLOBAL 数据项（所有权转移给 ClipDataObject）
    bool AddGlobalData(CLIPFORMAT cfFormat, HGLOBAL hGlobal) {
        if (!hGlobal || cfFormat == 0) {
            return false;
        }

        FORMATETC fmt{};
        fmt.cfFormat = cfFormat;
        fmt.ptd = nullptr;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;

        STGMEDIUM med{};
        med.tymed = TYMED_HGLOBAL;
        med.hGlobal = hGlobal;
        med.pUnkForRelease = nullptr;

        try {
            formats_.push_back(fmt);
            mediums_.push_back(med);
        } catch (...) {
            if (formats_.size() > mediums_.size()) {
                formats_.pop_back();
            }
            return false;
        }
        return true;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
            *ppvObject = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ::InterlockedIncrement(&refCount_);
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG count = ::InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* pFormatEtc, STGMEDIUM* pMedium) override {
        if (!pFormatEtc || !pMedium) {
            return E_INVALIDARG;
        }

        *pMedium = {};

        const int idx = FindFormatIndex(pFormatEtc);
        if (idx < 0) {
            return DV_E_FORMATETC;
        }

        const STGMEDIUM& srcMed = mediums_[static_cast<size_t>(idx)];
        if (srcMed.tymed != TYMED_HGLOBAL) {
            return DV_E_TYMED;
        }

        HGLOBAL hDup = DupGlobalMem(srcMed.hGlobal);
        if (!hDup) {
            return E_OUTOFMEMORY;
        }

        pMedium->tymed = TYMED_HGLOBAL;
        pMedium->hGlobal = hDup;
        pMedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    STDMETHODIMP GetDataHere(FORMATETC* /*pFormatEtc*/, STGMEDIUM* /*pMedium*/) override {
        return DATA_E_FORMATETC;
    }

    STDMETHODIMP QueryGetData(FORMATETC* pFormatEtc) override {
        if (!pFormatEtc) {
            return E_INVALIDARG;
        }
        return (FindFormatIndex(pFormatEtc) >= 0) ? S_OK : DV_E_FORMATETC;
    }

    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* pFormatEtcIn, FORMATETC* pFormatEtcOut) override {
        if (!pFormatEtcIn || !pFormatEtcOut) {
            return E_INVALIDARG;
        }
        *pFormatEtcOut = *pFormatEtcIn;
        pFormatEtcOut->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    // 支持 Windows Shell 在拖拽周期中写入拖拽上下文/反馈数据（如 DropDescription / Performed DropEffect）
    STDMETHODIMP SetData(FORMATETC* pFormatEtc, STGMEDIUM* pMedium, BOOL fRelease) override {
        if (!pFormatEtc || !pMedium) {
            return E_INVALIDARG;
        }
        if ((pFormatEtc->tymed & TYMED_HGLOBAL) == 0 || pMedium->tymed != TYMED_HGLOBAL) {
            return DV_E_TYMED;
        }
        if (!pMedium->hGlobal) {
            return E_INVALIDARG;
        }

        HGLOBAL hCopy = DupGlobalMem(pMedium->hGlobal);
        if (!hCopy) {
            return E_OUTOFMEMORY;
        }

        const int idx = FindFormatIndex(pFormatEtc);
        if (idx >= 0) {
            ::ReleaseStgMedium(&mediums_[static_cast<size_t>(idx)]);
            mediums_[static_cast<size_t>(idx)].tymed = TYMED_HGLOBAL;
            mediums_[static_cast<size_t>(idx)].hGlobal = hCopy;
            mediums_[static_cast<size_t>(idx)].pUnkForRelease = nullptr;
        } else {
            FORMATETC fmt{};
            fmt.cfFormat = pFormatEtc->cfFormat;
            fmt.ptd = nullptr;
            fmt.dwAspect = pFormatEtc->dwAspect ? pFormatEtc->dwAspect : DVASPECT_CONTENT;
            fmt.lindex = pFormatEtc->lindex;
            fmt.tymed = TYMED_HGLOBAL;

            STGMEDIUM med{};
            med.tymed = TYMED_HGLOBAL;
            med.hGlobal = hCopy;
            med.pUnkForRelease = nullptr;

            try {
                formats_.push_back(fmt);
                mediums_.push_back(med);
            } catch (...) {
                if (formats_.size() > mediums_.size()) {
                    formats_.pop_back();
                }
                ::GlobalFree(hCopy);
                return E_OUTOFMEMORY;
            }
        }

        if (fRelease) {
            ::ReleaseStgMedium(pMedium);
        }
        return S_OK;
    }

    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppEnumFormatEtc) override {
        if (!ppEnumFormatEtc) {
            return E_POINTER;
        }
        *ppEnumFormatEtc = nullptr;

        if (dwDirection == DATADIR_GET) {
            return ::SHCreateStdEnumFmtEtc(
                static_cast<UINT>(formats_.size()),
                formats_.data(),
                ppEnumFormatEtc);
        }
        return E_NOTIMPL;
    }

    STDMETHODIMP DAdvise(FORMATETC* /*pFormatEtc*/, DWORD /*advf*/, IAdviseSink* /*pAdvSink*/, DWORD* /*pdwConnection*/) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    STDMETHODIMP DUnadvise(DWORD /*dwConnection*/) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    STDMETHODIMP EnumDAdvise(IEnumSTATDATA** /*ppEnumAdvise*/) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~ClipDataObject() {
        for (auto& med : mediums_) {
            ::ReleaseStgMedium(&med);
        }
    }

    int FindFormatIndex(const FORMATETC* pFormatEtc) const noexcept {
        if (!pFormatEtc) {
            return -1;
        }
        for (size_t i = 0; i < formats_.size(); ++i) {
            const FORMATETC& candidate = formats_[i];
            if (pFormatEtc->cfFormat == candidate.cfFormat &&
                (pFormatEtc->tymed & candidate.tymed) != 0 &&
                pFormatEtc->dwAspect == candidate.dwAspect &&
                (pFormatEtc->lindex == candidate.lindex || pFormatEtc->lindex == -1)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    volatile LONG refCount_ = 1;
    std::vector<FORMATETC> formats_;
    std::vector<STGMEDIUM> mediums_;
};

} // namespace

bool OleDragDrop::PerformDragDrop(const RawClipboardPayload& payload, const std::wstring& imageFullPath) {
    // 入参校验契约：
    // 1. 当 payload.type == ClipItemType::Image 时，图片拖拽直接通过 imageFullPath 构造 CF_HDROP，
    //    明确允许 payload.pngBytes 为空（无需从磁盘加载完整 PNG 字节流），仅校验 imageFullPath 非空且磁盘文件实际存在（非目录）。
    // 2. 当 payload.type != ClipItemType::Image 时，要求 plainText / htmlBytes / rtfBytes 至少有一项非空。
    if (payload.type == ClipItemType::Image) {
        if (imageFullPath.empty()) {
            return false;
        }
        const DWORD attrs = ::GetFileAttributesW(imageFullPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return false;
        }
    } else {
        if (payload.plainText.empty() && payload.htmlBytes.empty() && payload.rtfBytes.empty()) {
            return false;
        }
    }

    auto* pDataObject = new (std::nothrow) ClipDataObject();
    if (!pDataObject) {
        return false;
    }

    auto* pDropSource = new (std::nothrow) ClipDropSource();
    if (!pDropSource) {
        pDataObject->Release();
        return false;
    }

    bool setupSuccess = false;

    if (payload.type == ClipItemType::Image) {
        // 1. 若图片项附带原始富文本结构（如从 Word 复制的无字图形/图注），优先挂载富文本格式，
        //    确保拖入 Word 等富文本编辑器时保留原富文本结构，而拖入文件夹/资源管理器时使用 CF_HDROP 生成 PNG 文件。
        if (!payload.htmlBytes.empty()) {
            const UINT cfHtml = ::RegisterClipboardFormatW(L"HTML Format");
            if (cfHtml != 0) {
                HGLOBAL hHtml = CreateNullTerminatedBytesGlobal(payload.htmlBytes);
                if (hHtml) {
                    if (pDataObject->AddGlobalData(static_cast<CLIPFORMAT>(cfHtml), hHtml)) {
                        setupSuccess = true;
                    } else {
                        ::GlobalFree(hHtml);
                    }
                }
            }
        }

        if (!payload.rtfBytes.empty()) {
            const UINT cfRtf = ::RegisterClipboardFormatW(L"Rich Text Format");
            if (cfRtf != 0) {
                HGLOBAL hRtf = CreateNullTerminatedBytesGlobal(payload.rtfBytes);
                if (hRtf) {
                    if (pDataObject->AddGlobalData(static_cast<CLIPFORMAT>(cfRtf), hRtf)) {
                        setupSuccess = true;
                    } else {
                        ::GlobalFree(hRtf);
                    }
                }
            }
        }

        // 2. 构造 CF_HDROP 的 HGLOBAL
        HGLOBAL hDrop = CreateHDropGlobal(imageFullPath);
        if (hDrop) {
            if (pDataObject->AddGlobalData(CF_HDROP, hDrop)) {
                setupSuccess = true;
            } else {
                ::GlobalFree(hDrop);
            }
        }

        // 3. 注册并写入 "Preferred DropEffect" = DROPEFFECT_COPY
        if (setupSuccess) {
            const UINT cfPreferredDropEffect = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
            if (cfPreferredDropEffect != 0) {
                HGLOBAL hEffect = CreateDwordGlobal(DROPEFFECT_COPY);
                if (hEffect) {
                    if (!pDataObject->AddGlobalData(static_cast<CLIPFORMAT>(cfPreferredDropEffect), hEffect)) {
                        ::GlobalFree(hEffect);
                    }
                }
            }
        }
    } else {
        // 文本或富文本拖拽封装
        // 1. 构造 CF_UNICODETEXT 的 HGLOBAL
        HGLOBAL hText = CreateUnicodeTextGlobal(payload.plainText);
        if (hText) {
            if (pDataObject->AddGlobalData(CF_UNICODETEXT, hText)) {
                setupSuccess = true;
            } else {
                ::GlobalFree(hText);
            }
        }

        // 2. 若 htmlBytes 非空，同时注册 "HTML Format" 并挂载 HGLOBAL（末尾确保有 '\0'）
        if (!payload.htmlBytes.empty()) {
            const UINT cfHtml = ::RegisterClipboardFormatW(L"HTML Format");
            if (cfHtml != 0) {
                HGLOBAL hHtml = CreateNullTerminatedBytesGlobal(payload.htmlBytes);
                if (hHtml) {
                    if (pDataObject->AddGlobalData(static_cast<CLIPFORMAT>(cfHtml), hHtml)) {
                        setupSuccess = true;
                    } else {
                        ::GlobalFree(hHtml);
                    }
                }
            }
        }

        // 3. 若 rtfBytes 非空，同时注册 "Rich Text Format" 并挂载 HGLOBAL（末尾确保有 '\0'）
        if (!payload.rtfBytes.empty()) {
            const UINT cfRtf = ::RegisterClipboardFormatW(L"Rich Text Format");
            if (cfRtf != 0) {
                HGLOBAL hRtf = CreateNullTerminatedBytesGlobal(payload.rtfBytes);
                if (hRtf) {
                    if (pDataObject->AddGlobalData(static_cast<CLIPFORMAT>(cfRtf), hRtf)) {
                        setupSuccess = true;
                    } else {
                        ::GlobalFree(hRtf);
                    }
                }
            }
        }
    }

    bool dropSucceeded = false;
    if (setupSuccess) {
        DWORD dwEffect = DROPEFFECT_NONE;
        // 严格只传 DROPEFFECT_COPY，从系统底层彻底禁止资源管理器在同盘拖拽时执行移动（剪切）操作
        const HRESULT hr = ::DoDragDrop(pDataObject, pDropSource, DROPEFFECT_COPY, &dwEffect);
        dropSucceeded = (hr == DRAGDROP_S_DROP && (dwEffect & DROPEFFECT_COPY) != 0);
    }

    g_isOverInternalTarget = false;
    pDropSource->Release();
    pDataObject->Release();
    return dropSucceeded;
}

bool OleDragDrop::RegisterInternalDropTarget(HWND hwnd, InternalDropTargetCallbacks callbacks) {
    if (!hwnd || !::IsWindow(hwnd)) {
        return false;
    }
    auto* target = new (std::nothrow) InternalWindowDropTarget(std::move(callbacks));
    if (!target) {
        return false;
    }
    const HRESULT hr = ::RegisterDragDrop(hwnd, target);
    target->Release();
    return SUCCEEDED(hr);
}

void OleDragDrop::RevokeInternalDropTarget(HWND hwnd) {
    g_isOverInternalTarget = false;
    if (hwnd && ::IsWindow(hwnd)) {
        ::RevokeDragDrop(hwnd);
    }
}
