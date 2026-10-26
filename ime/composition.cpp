//
// M5 拼音模式：组合窗口（composition）生命周期与键分发。
//
// 设计：zh-CN profile 激活 → _pinyinMode=true（DetectPinyinMode，ActivateEx 调用）。
// 字母键进入组合（ITfComposition，文本以拼音字母形式内联显示），
// 空格/数字把组合文本整体替换为汉字候选并 EndComposition，
// Enter 保留原字母，Esc 删除取消。en-US profile 保持英文直通+预测。
//
#include "service.h"
#include "editsession.h"

#include "../common/proto.h"
#include "../common/guids.h"
#include "../common/log.h"

#include <functional>

namespace lingxi {

// 通用 lambda 编辑会话（组合编辑 + 组合内读操作共用）
class CPinyinEditSession : public CEditSessionBase
{
public:
    CPinyinEditSession(CTextService* pSvc, ITfContext* pCtx,
                       std::function<HRESULT(TfEditCookie)> task)
        : CEditSessionBase(pSvc, pCtx), _task(std::move(task)) {}

    STDMETHOD(DoEditSession)(TfEditCookie ec) override { return _task(ec); }

private:
    std::function<HRESULT(TfEditCookie)> _task;
};

// 同步优先编辑会话（与 service.cpp 的 RequestSession 相同语义）
static HRESULT RequestSessionEx(ITfContext* pCtx, TfClientId cid, ITfEditSession* pSes, DWORD baseFlags)
{
    HRESULT hrSession = E_FAIL;
    HRESULT hr = pCtx->RequestEditSession(cid, pSes, baseFlags | TF_ES_SYNC, &hrSession);
    if (hr == TS_E_SYNCHRONOUS || hrSession == TS_E_SYNCHRONOUS)
        pCtx->RequestEditSession(cid, pSes, baseFlags | TF_ES_ASYNCDONTCARE, &hrSession);
    return SUCCEEDED(hr) ? hrSession : hr;
}

static bool GetFocusContext(ITfThreadMgr* pTm, ITfContext** ppCtx)
{
    if (!pTm) return false;
    ITfDocumentMgr* pDoc = nullptr;
    if (FAILED(pTm->GetFocus(&pDoc)) || !pDoc) return false;
    HRESULT hr = pDoc->GetTop(ppCtx);
    pDoc->Release();
    return SUCCEEDED(hr) && *ppCtx;
}

static bool IsLetterKey(WPARAM wParam)
{
    return (wParam >= L'a' && wParam <= L'z') || (wParam >= L'A' && wParam <= L'Z');
}

// ==================== 模式检测 ====================

void CTextService::DetectPinyinMode()
{
    _pinyinMode = false;
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfiles, (void**)&pProfiles)))
    {
        LANGID lang = 0;
        GUID guidProfile{};
        // 查询"我们这个 TIP"当前激活的 profile：zh-CN 配置 → 拼音模式
        if (SUCCEEDED(pProfiles->GetActiveLanguageProfile(CLSID_LingxiTextService,
                                                         &lang, &guidProfile)) &&
            IsEqualGUID(guidProfile, GUID_LingxiProfile_zhCN))
        {
            _pinyinMode = true;
        }
        pProfiles->Release();
    }
    LX_LOG(L"ime", L"拼音模式: %d", (int)_pinyinMode);
}

// ==================== 组合编辑原语 ====================

void CTextService::PinyinStart(wchar_t ch)
{
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx)) return;

    _pinyin.clear();
    _pinyin += ch;

    _fSelfInsert = TRUE; // 组合编辑引发的 OnEndEdit 全部抑制（我们自己发 SendPinyinCtx）
    ITfContext* pCtxSession = pCtx;
    auto task = [this, pCtxSession](TfEditCookie ec) -> HRESULT
    {
        ITfInsertAtSelection* pInsert = nullptr;
        if (FAILED(pCtxSession->QueryInterface(IID_ITfInsertAtSelection, (void**)&pInsert)))
            return E_FAIL;
        ITfRange* pRange = nullptr;
        HRESULT hr = pInsert->InsertTextAtSelection(ec, 0, _pinyin.c_str(), (LONG)_pinyin.size(), &pRange);
        pInsert->Release();
        if (FAILED(hr) || !pRange) return FAILED(hr) ? hr : E_FAIL;
        _compRange = pRange; // 接管引用

        ITfContextComposition* pCc = nullptr;
        if (SUCCEEDED(pCtxSession->QueryInterface(IID_ITfContextComposition, (void**)&pCc)))
        {
            pCc->StartComposition(ec, pRange, (ITfCompositionSink*)this, &_pComposition);
            pCc->Release();
        }
        pRange->Release();
        if (_pComposition)
            LX_LOG(L"ime", L"组合开始 pinyin=%ls", _pinyin.c_str());
        return S_OK;
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    _fSelfInsert = FALSE;
    SendPinyinCtx();
}

void CTextService::PinyinAppend(wchar_t ch)
{
    if (!_pComposition) return;
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx)) return;

    _pinyin += ch;
    _fSelfInsert = TRUE;
    auto task = [this](TfEditCookie ec) -> HRESULT
    {
        if (!_compRange) return E_FAIL;
        return _compRange->SetText(ec, 0, _pinyin.c_str(), (LONG)_pinyin.size());
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    _fSelfInsert = FALSE;
    SendPinyinCtx();
}

void CTextService::PinyinShrink()
{
    if (!_pComposition) return;
    if (_pinyin.size() <= 1)
    {
        PinyinCancel(); // 删光了 → 取消组合
        return;
    }
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx)) return;

    _pinyin.pop_back();
    _fSelfInsert = TRUE;
    auto task = [this](TfEditCookie ec) -> HRESULT
    {
        if (!_compRange) return E_FAIL;
        return _compRange->SetText(ec, 0, _pinyin.c_str(), (LONG)_pinyin.size());
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    _fSelfInsert = FALSE;
    SendPinyinCtx();
}

// replaceWith 语义：nullptr=保留现有文本；空串=删除；其他=替换
void CTextService::PinyinEndComposition(ITfContext* pCtx, const std::wstring* replaceWith)
{
    if (!_pComposition && !_compRange) return;
    _fSelfInsert = TRUE;
    auto task = [this, replaceWith](TfEditCookie ec) -> HRESULT
    {
        if (_compRange && replaceWith)
        {
            HRESULT hr = _compRange->SetText(ec, 0, replaceWith->c_str(), (LONG)replaceWith->size());
            if (FAILED(hr)) return hr;
        }
        if (_pComposition) return _pComposition->EndComposition(ec);
        return S_OK;
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    _fSelfInsert = FALSE;

    if (_compRange) { _compRange->Release(); _compRange = nullptr; }
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    _pinyin.clear();
}

void CTextService::PinyinCancel()
{
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx))
    {
        // 拿不到焦点上下文（失焦瞬间）：只清状态，组合由 OnCompositionTerminated 兜底
        if (_compRange) { _compRange->Release(); _compRange = nullptr; }
        if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
        _pinyin.clear();
        return;
    }
    std::wstring empty;
    PinyinEndComposition(pCtx, &empty);
    pCtx->Release();
}

void CTextService::PinyinCommitHanzi(const std::wstring& hanzi)
{
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx))
    {
        LX_LOG(L"ime", L"提交汉字失败：无焦点上下文");
        return;
    }
    LX_LOG(L"ime", L"组合提交汉字 chars=%u", (uint32_t)hanzi.size());
    PinyinEndComposition(pCtx, &hanzi);
    pCtx->Release();
    ClearCandidates();
    SendHide();
}

void CTextService::PinyinCommitRaw()
{
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx)) return;
    PinyinEndComposition(pCtx, nullptr); // 保留拼音字母原文
    pCtx->Release();
    ClearCandidates();
    SendHide();
}

// ==================== 上下文发送（拼音串 + 光标矩形） ====================

void CTextService::SendPinyinCtx()
{
    if (!_compRange) return;
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx)) return;

    RECT rc{};
    ITfContext* pCtxSession = pCtx;
    auto task = [this, pCtxSession, &rc](TfEditCookie ec) -> HRESULT
    {
        if (!_compRange) return E_FAIL;
        ITfContextView* pView = nullptr;
        if (SUCCEEDED(pCtxSession->GetActiveView(&pView)) && pView)
        {
            BOOL clipped = FALSE;
            pView->GetTextExt(ec, _compRange, &rc, &clipped);
            pView->Release();
        }
        return S_OK;
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READ);
    pSession->Release();
    pCtx->Release();

    SendCtx(_pinyin, rc, CTX_F_PINYIN);
}

// ==================== 键分发 ====================

bool CTextService::PinyinKeyEaten(WPARAM wParam)
{
    if (!_pinyinMode) return false;
    if (_pComposition)
    {
        if (IsLetterKey(wParam)) return true;
        switch (wParam)
        {
        case VK_BACK: case VK_ESCAPE: case VK_RETURN:
        case VK_SPACE: case VK_TAB:
            return true;
        }
        if (wParam >= L'1' && wParam <= L'9') return CandidatesVisible();
        return false; // 标点等：提交拼音原文后放行
    }
    return IsLetterKey(wParam); // 非组合态：字母启动组合
}

bool CTextService::GetCandidate(uint32_t index, std::wstring& out)
{
    if (!_csInit) return false;
    EnterCriticalSection(&_cs);
    bool ok = (_verCands == _verSent && index < _cands.size());
    if (ok) out = _cands[index];
    LeaveCriticalSection(&_cs);
    return ok;
}

void CTextService::HandlePinyinKey(ITfContext* pContext, WPARAM wParam, BOOL* pfEaten)
{
    *pfEaten = TRUE;
    wchar_t ch = (wParam >= L'A' && wParam <= L'Z') ? (wchar_t)(wParam + 32) : (wchar_t)wParam;

    if (!_pComposition)
    {
        PinyinStart(ch); // 键盘判定已保证是字母
        return;
    }

    if (IsLetterKey(wParam))
    {
        PinyinAppend(ch);
        return;
    }
    switch (wParam)
    {
    case VK_BACK:
        PinyinShrink();
        return;
    case VK_ESCAPE:
        PinyinCancel();
        ClearCandidates();
        SendHide();
        return;
    case VK_RETURN:
        PinyinCommitRaw();
        ClearCandidates();
        SendHide();
        return;
    case VK_SPACE:
    case VK_TAB:
    {
        std::wstring hanzi;
        if (GetCandidate(0, hanzi)) PinyinCommitHanzi(hanzi);
        else                        PinyinCommitRaw();
        return;
    }
    default:
        if (wParam >= L'1' && wParam <= L'9')
        {
            std::wstring hanzi;
            if (GetCandidate((uint32_t)(wParam - L'1'), hanzi)) PinyinCommitHanzi(hanzi);
            else                                                PinyinCommitRaw();
            return;
        }
        // 标点/其他：提交拼音原文，按键本身放行给应用
        PinyinCommitRaw();
        ClearCandidates();
        SendHide();
        *pfEaten = FALSE;
        return;
    }
}

// ==================== ITfCompositionSink ====================

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition)
{
    // 宿主强制终止（失焦/文档关闭等）：组合文本保留在文档中，清理内部状态
    LX_LOG(L"ime", L"组合被宿主终止");
    if (_compRange) { _compRange->Release(); _compRange = nullptr; }
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    _pinyin.clear();
    ClearCandidates();
    return S_OK;
}

} // namespace lingxi
