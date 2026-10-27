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

// SEH 包裹：模式检测涉及 TSF 查询，任何异常不能带崩宿主。
// 调用时机：按键路径（OnTestKeyDown/OnKeyDown）惰性触发——全部 TSF 调用
// 留在宿主 UI 线程，远离 CTF 激活回调窗口（见 PITFALLS 启动崩溃条目）。
BOOL DetectPinyinModeSEH(CTextService* svc)
{
    __try
    {
        svc->DetectPinyinMode();
        return TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
}

// 按键路径入口：语言变化（GetKeyboardLayout，纯 user32 查询）或脏标记 → 惰性检测。
// 全部信号零 CTF 重入；GetForeground 仅在真正检测时调用一次。
void CTextService::MaybeRedetectPinyinMode()
{
    LANGID cur = LOWORD((UINT_PTR)GetKeyboardLayout(0));
    if (cur != _lastLangid)
    {
        _lastLangid = cur;
        _modeDirty.store(true);
    }
    if (_modeDirty.exchange(false))
        DetectPinyinModeSEH(this);
}

// SEH 包裹拼音组合键处理：组合编辑会话同步跑在 CTF/TSF 框架内部，
// 语言切换/文档拆除的过渡窗口里框架可能内部 AV（实测 textinputframework.dll
// 读 -1 指针，见 PITFALLS）。遏制之：安全复位组合状态并降级回直通模式。
void HandlePinyinKeySEH(CTextService* svc, ITfContext* pContext, WPARAM wParam, BOOL* pfEaten)
{
    __try
    {
        svc->HandlePinyinKey(pContext, wParam, pfEaten);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *pfEaten = TRUE; // 异常键吃掉，不透传半处理状态给应用
        svc->AbortCompositionState();
        LX_LOG(L"ime", L"组合键处理异常 0x%08X，已复位并降级直通",
               (uint32_t)GetExceptionCode());
    }
}

void CTextService::DetectPinyinMode()
{
    // 拼音模式 = 前台键盘 TIP 是我们 && 当前输入语言是中文（zh-CN profile）。
    // 全部用只读查询（GetForeground + GetKeyboardLayout）：
    // ⚠️ 不要在按键路径上调 GetActiveLanguageProfile —— 它会触发 CTF 输入
    // 上下文重算（实测：按键分发内调用导致 sandwich 上下文重建、GetTextExt
    // 失效、候选窗锚点丢失，见 PITFALLS）。
    // 默认启用；LINGXI_PINYIN=0 可退回纯直通（zh-CN 不再进组合）。
    bool py = false;
    wchar_t flag[8] = L"";
    bool disabled = GetEnvironmentVariableW(L"LINGXI_PINYIN", flag, 8) > 0 && flag[0] == L'0';
    if (!disabled && _pThreadMgr)
    {
        ITfKeystrokeMgr* pKm = nullptr;
        CLSID fg{};
        if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm)))
        {
            if (SUCCEEDED(pKm->GetForeground(&fg)) && IsEqualCLSID(fg, CLSID_LingxiTextService))
                py = (LOWORD((UINT_PTR)GetKeyboardLayout(0)) == 0x0804);
            pKm->Release();
        }
    }
    if (py != _pinyinMode)
        LX_LOG(L"ime", L"拼音模式: %d (lang=0x%04X)", (int)py,
               (unsigned)LOWORD((UINT_PTR)GetKeyboardLayout(0))); // 仅变化时记录
    _pinyinMode = py;
}

// ==================== 组合编辑原语 ====================

void CTextService::PinyinStart(wchar_t ch)
{
    // 去激活/模式切换过渡期绝不启组合（此时文档栈可能正被 CTF 重建）
    if (!_fActivated || !_pinyinMode) return;
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

void CTextService::AbortCompositionState()
{
    // 组合操作中发生框架级异常：对象状态可疑，不 Release（交给框架/进程清理），
    // 只断开跟踪并降级直通，避免后续键继续在坏状态上操作。
    _compRange = nullptr;
    _pComposition = nullptr;
    _pinyin.clear();
    _fSelfInsert = FALSE;
    _pinyinMode = false; // 降级：下次焦点/激活事件重置脏标记后可重新启用
    _modeDirty.store(true);
}

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
