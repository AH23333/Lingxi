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
#include "../common/settings.h"

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

// 安全取光标矩形。Chromium 上对"写会话里产生的 composition range"在另一个
// 读会话里调用 GetTextExt 会触发框架内部 AV（0xC0000005）。这里：
//   1) 优先用当前读会话 GetSelection 得到的插入点 range（与 ec 同会话，最安全）；
//   2) 整段 SEH 包裹，任何宿主 AV 都退化为"无矩形"（候选窗降级到系统插入符）。
// 注意：本函数只用 POD 与 COM 裸指针（手动 Release），无 C++ 展开对象，可用 __try。
static HRESULT SafeGetTextExt(ITfContextView* pView, ITfContext* pCtx, TfEditCookie ec,
                              ITfRange* pCompRange, RECT* prc)
{
    __try
    {
        ITfRange* pRange = nullptr;
        TF_SELECTION tfSel{};
        ULONG fetched = 0;
        HRESULT hs = pCtx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSel, &fetched);
        if (SUCCEEDED(hs) && fetched == 1 && tfSel.range)
        {
            pRange = tfSel.range; // 由我们 Release
        }
        else if (pCompRange)
        {
            pRange = pCompRange; // 不 AddRef，仅本函数内同步使用
        }
        if (!pRange) return E_FAIL;

        BOOL clipped = FALSE;
        RECT r{};
        HRESULT he = pView->GetTextExt(ec, pRange, &r, &clipped);
        if (tfSel.range) tfSel.range->Release();
        if (SUCCEEDED(he))
        {
            if (r.right > r.left || r.bottom > r.top) { *prc = r; }
            return S_OK;
        }
        return he;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return E_UNEXPECTED;
    }
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

// 按键路径入口：语言变化或设置开关变化（若发生）→ 投递延迟检测；
// 检测本身绝不在按键分发内执行。两个探测都是 µs 级只读查询。
void CTextService::MaybeRedetectPinyinMode()
{
    LANGID cur = LOWORD((UINT_PTR)GetKeyboardLayout(0));
    if (cur != _lastLangid)
    {
        _lastLangid = cur;
        RequestModeDetect();
        return;
    }
    Settings st = LoadSettings(); // 托盘/设置页的开关改动即时生效
    if (st.pinyinEnabled != _cachedPinyinEnabled)
    {
        _cachedPinyinEnabled = st.pinyinEnabled;
        RequestModeDetect();
    }
}

// 置脏 + 投递封送消息：检测在消息泵空闲点执行（WM_APP_LX_MODE），
// 远离 CTF 激活回调窗口与按键分发（两者的 TSF 查询都出过事故，见 PITFALLS #17/#18）
void CTextService::RequestModeDetect()
{
    _modeDirty.store(true);
    if (_hwndMarshal)
        PostMessageW(_hwndMarshal, WM_APP_LX_MODE, 0, 0);
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
    // 拼音模式 = 前台键盘 TIP 是我们 && 我们 TIP 的激活档案是 zh-CN && 设置未关闭。
    // ⚠️ 两个铁律（PITFALLS #17/#18）：
    //   a) 本函数只在封送窗口消息（消息泵空闲点）里跑，绝不在按键分发或激活回调内；
    //   b) 判据用 GetActiveLanguageProfile 的真实激活档案——纯 TSF TIP 切换
    //      profile 时线程 HKL 不变，GetKeyboardLayout 判不了语言。
    // 开关读用户设置（托盘/设置页，HKCU\Software\LingxiIme），实时生效。
    bool py = false;
    bool disabled = !LoadSettings().pinyinEnabled;
    if (!disabled && _pThreadMgr)
    {
        ITfKeystrokeMgr* pKm = nullptr;
        CLSID fg{};
        bool weForeground = SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm)) &&
                            SUCCEEDED(pKm->GetForeground(&fg)) && IsEqualCLSID(fg, CLSID_LingxiTextService);
        if (pKm) pKm->Release();
        if (weForeground)
        {
            ITfInputProcessorProfiles* pProfiles = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                                           IID_ITfInputProcessorProfiles, (void**)&pProfiles)))
            {
                LANGID lang = 0;
                GUID guidProfile{};
                if (SUCCEEDED(pProfiles->GetActiveLanguageProfile(CLSID_LingxiTextService,
                                                                  &lang, &guidProfile)) &&
                    IsEqualGUID(guidProfile, GUID_LingxiProfile_zhCN))
                {
                    py = true;
                }
                pProfiles->Release();
            }
        }
    }
    if (py != _pinyinMode)
        LX_LOG(L"ime", L"拼音模式: %d", (int)py); // 仅变化时记录
    _pinyinMode = py;
}

// ==================== 组合编辑原语 ====================

void CTextService::PinyinStart(wchar_t ch)
{
    // 去激活/模式切换过渡期绝不启组合（此时文档栈可能正被 CTF 重建）
    if (!_fActivated || !_pinyinMode) return;

    // 无组合模式：拼音字母不写入宿主，只在候选窗显示，宿主永不产生 composition。
    if (!_inlineComp)
    {
        _pinyin.clear();
        _pinyin += ch;
        LX_LOG(L"key", L"Start(no-inline) ch=%lc", (wint_t)ch);
        ClearCandidates();
        SendPinyinCtx();
        return;
    }

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
    HRESULT hrs = RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    if (FAILED(hrs))
        LX_LOG(L"ime", L"启动组合会话失败 0x%08X", hrs);
    pSession->Release();
    pCtx->Release();
    _fSelfInsert = FALSE;
    if (_pComposition) SendPinyinCtx();
}

void CTextService::PinyinAppend(wchar_t ch)
{
    if (!_inlineComp)
    {
        // 无组合模式：仅更新内存拼音串并刷新候选窗。
        _pinyin += ch;
        LX_LOG(L"key", L"Append(no-inline) ch=%lc len=%u", (wint_t)ch, (uint32_t)_pinyin.size());
        SendPinyinCtx();
        return;
    }
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
    if (!_inlineComp)
    {
        LX_LOG(L"key", L"Shrink(no-inline) before len=%u", (uint32_t)_pinyin.size());
        if (_pinyin.empty()) return;
        _pinyin.pop_back();
        if (_pinyin.empty()) { LX_LOG(L"key", L"Shrink -> Cancel"); PinyinCancel(); return; }
        LX_LOG(L"key", L"Shrink after len=%u", (uint32_t)_pinyin.size());
        SendPinyinCtx();
        return;
    }
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
    // 值捕获替换串：会话可能异步执行，不能引用调用方的局部字符串指针。
    bool doReplace = (replaceWith != nullptr);
    std::wstring replaceText = replaceWith ? *replaceWith : std::wstring();
    _fSelfInsert = TRUE;
    auto task = [this, doReplace, replaceText](TfEditCookie ec) -> HRESULT
    {
        LX_LOG(L"ime", L"[end] enter task range=%p comp=%p doReplace=%d",
               (void*)_compRange, (void*)_pComposition, (int)doReplace);
        if (_compRange && doReplace)
        {
            HRESULT hr = _compRange->SetText(ec, 0, replaceText.c_str(), (LONG)replaceText.size());
            LX_LOG(L"ime", L"[end] SetText=0x%08X", hr);
            if (FAILED(hr)) return hr;
        }
        if (_pComposition)
        {
            HRESULT he = _pComposition->EndComposition(ec);
            LX_LOG(L"ime", L"[end] EndComposition=0x%08X", he);
            return he;
        }
        return S_OK;
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    HRESULT hrs = RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    LX_LOG(L"ime", L"[end] RequestSession=0x%08X", hrs);
    pSession->Release();
    pCtx->Release();
    _fSelfInsert = FALSE;

    if (_compRange) { _compRange->Release(); _compRange = nullptr; }
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    _pinyin.clear();
}

// 无组合模式通用收尾：清空内存拼音/候选并隐藏候选窗（宿主没有任何组合需要结束）。
void CTextService::PinyinResetNoInline()
{
    _pinyin.clear();
    ClearCandidates();
    SendHide();
}

void CTextService::PinyinCancel()
{
    if (!_inlineComp)
    {
        // 无组合：拼音字母从未写入宿主，直接丢弃即可。
        PinyinResetNoInline();
        return;
    }
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
    if (!_inlineComp)
    {
        // 无组合：宿主里没有拼音字母，直接把汉字插到光标处。
        LX_LOG(L"ime", L"无组合上屏汉字 chars=%u", (uint32_t)hanzi.size());
        _fSelfInsert = TRUE;
        InsertTextAtSelection(hanzi);
        _fSelfInsert = FALSE;
        PinyinResetNoInline();
        return;
    }
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
    if (!_inlineComp)
    {
        // 无组合：回车把原始拼音字母作为普通文本上屏。
        if (!_pinyin.empty())
        {
            std::wstring raw = _pinyin;
            _fSelfInsert = TRUE;
            InsertTextAtSelection(raw);
            _fSelfInsert = FALSE;
        }
        PinyinResetNoInline();
        return;
    }
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
    if (_inlineComp && !_compRange) return;
    if (!_inlineComp && _pinyin.empty()) return;
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx)) return;

    // ⚠️ 整个 rect 计算 + 发送都放进编辑会话内，并对 pinyin 做值拷贝。
    // Chromium 上 TF_ES_SYNC 常回退为异步会话：DoEditSession 会在本函数返回后
    // 才执行，此时任何"引用捕获的局部栈变量"(&rc/&hasRect)都已销毁，写入即 AV。
    // 编辑会话由 TSF 在宿主 UI 线程回调，故 GetGUIThreadInfo/SendCtx 仍在 UI 线程。
    bool inlineComp = _inlineComp;
    std::wstring pinyin = _pinyin;
    ITfContext* pCtxSession = pCtx;
    auto task = [this, pCtxSession, pinyin, inlineComp](TfEditCookie ec) -> HRESULT
    {
        if (inlineComp && !_compRange) return E_FAIL;
        RECT rc{};
        bool hasRect = false;
        ITfContextView* pView = nullptr;
        if (SUCCEEDED(pCtxSession->GetActiveView(&pView)) && pView)
        {
            RECT rcText{};
            // 无组合时传 nullptr：用当前插入点选区定位（最安全，不触碰组合 range）。
            ITfRange* pRangeForRect = inlineComp ? _compRange : nullptr;
            HRESULT he = SafeGetTextExt(pView, pCtxSession, ec, pRangeForRect, &rcText);
            if (SUCCEEDED(he)) { rc = rcText; hasRect = true; }
            else if (he == E_UNEXPECTED)
                LX_LOG(L"ime", L"GetTextExt 宿主异常，已降级系统插入符");
            pView->Release();
        }
        if (!hasRect)
        {
            // 降级：系统插入符（自绘光标应用可能拿不到，零 rect 由 UI 保持原位）
            GUITHREADINFO gti{ sizeof(gti) };
            if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret)
            {
                POINT pt = { gti.rcCaret.left, gti.rcCaret.bottom };
                ClientToScreen(gti.hwndCaret, &pt);
                rc = { pt.x, pt.y, pt.x, pt.y };
            }
        }
        SendCtx(pinyin, rc, CTX_F_PINYIN);
        return S_OK;
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READ);
    pSession->Release();
    pCtx->Release();
}

// ==================== 键分发 ====================

bool CTextService::PinyinKeyEaten(WPARAM wParam)
{
    if (!_pinyinMode) return false;
    // 中英混排：Shift 按下或 CapsLock 开启时，字母直通（输入大写/英文）
    bool shiftOrCaps = (GetKeyState(VK_SHIFT) & 0x8000) != 0 || (GetKeyState(VK_CAPITAL) & 1) != 0;
    if (PinyinActive())
    {
        if (IsLetterKey(wParam))
        {
            // 输入中的 Shift+字母：也要吃（HandlePinyinKey 会先提交原文再放行）
            return true;
        }
        if (shiftOrCaps && IsLetterKey(wParam)) return true; // 同上（不可达，防御）
        switch (wParam)
        {
        case VK_BACK: case VK_ESCAPE: case VK_RETURN:
        case VK_SPACE: case VK_TAB:
            return true;
        case VK_PRIOR: case VK_NEXT: // PageUp/PageDown：输入中翻页（吃）
            return CandidatesVisible();
        case VK_OEM_MINUS: case VK_OEM_PLUS: // - / =：仅在有多页时作翻页，否则当标点
        {
            bool multi = false;
            EnterCriticalSection(&_cs);
            multi = _cands.size() > kPageSize && _verCands == _verSent;
            LeaveCriticalSection(&_cs);
            return multi;
        }
        }
        if (wParam >= L'1' && wParam <= L'9') return CandidatesVisible();
        return false; // 标点等：提交后放行
    }
    // 未开始输入：Shift/Caps 下的字母直通，普通字母启动拼音
    if (shiftOrCaps && IsLetterKey(wParam)) return false;
    return IsLetterKey(wParam);
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
    LX_LOG(L"key", L"HandlePinyinKey vk=%u('0x%02X') active=%d pinyinLen=%u",
           (uint32_t)wParam, (uint32_t)wParam, (int)PinyinActive(), (uint32_t)_pinyin.size());

    // 中英混排：Shift/CapsLock + 字母 → 提交当前拼音（保留字母原文），大写字母放行
    bool shiftOrCaps = (GetKeyState(VK_SHIFT) & 0x8000) != 0 || (GetKeyState(VK_CAPITAL) & 1) != 0;
    if (shiftOrCaps && IsLetterKey(wParam))
    {
        if (PinyinActive())
        {
            PinyinCommitRaw();
            ClearCandidates();
            SendHide();
        }
        *pfEaten = FALSE; // 字母按 Shift 状态由应用上屏
        return;
    }

    // 非活动态：只有字母键才启动拼音；退格/标点/数字等一切非字母键直接放行，
    // 绝不能吞掉退格（否则已上屏文字无法删除）或把控制字符当拼音。
    if (!PinyinActive())
    {
        if (!IsLetterKey(wParam))
        {
            if (wParam == VK_BACK)
                _lastBackspaceTick = GetTickCount64(); // 退格删除：抑制随之而来的补全预测
            *pfEaten = FALSE;
            return;
        }
        wchar_t ch0 = (wchar_t)((wParam >= L'A' && wParam <= L'Z') ? (wParam + 32) : wParam);
        PinyinStart(ch0);
        return;
    }

    if (IsLetterKey(wParam))
    {
        wchar_t ch = (wchar_t)((wParam >= L'A' && wParam <= L'Z') ? (wParam + 32) : wParam);
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
    // 翻页：PageDown / = 下一页，PageUp / - 上一页
    case VK_NEXT:
    case VK_OEM_PLUS:
        PageCandidates(+1);
        return;
    case VK_PRIOR:
    case VK_OEM_MINUS:
        PageCandidates(-1);
        return;
    case VK_SPACE:
    case VK_TAB:
    {
        std::wstring hanzi;
        if (GetCandidate(_pageStart, hanzi)) PinyinCommitHanzi(hanzi);
        else                                 PinyinCommitRaw();
        return;
    }
    default:
        if (wParam >= L'1' && wParam <= L'9')
        {
            std::wstring hanzi;
            uint32_t idx = _pageStart + (uint32_t)(wParam - L'1');
            if (GetCandidate(idx, hanzi)) PinyinCommitHanzi(hanzi);
            else                          PinyinCommitRaw();
            return;
        }
        // 标点/其他：优先提交汉字首选（标点随后放行），无候选则保留字母原文
        {
            std::wstring hanzi;
            if (GetCandidate(0, hanzi)) PinyinCommitHanzi(hanzi);
            else                        PinyinCommitRaw();
            ClearCandidates();
            SendHide();
            *pfEaten = FALSE;
        }
        return;
    }
}

// ==================== ITfCompositionSink ====================

// 崩溃恢复内层（含 C++ 对象，可能再次触发宿主 AV，外层必须 SEH 包裹）。
// 结束宿主里的悬挂组合并释放本次组合持有的引用。
void CTextService::AbortHostCompositionInner(ITfRange* pRange, ITfComposition* pComp)
{
    if (pComp)
    {
        ITfContext* pCtx = nullptr;
        if (GetFocusContext(_pThreadMgr, &pCtx))
        {
            // 写会话：删字母 + EndComposition，把宿主拉回非组合态。
            auto* pSession = new CAbortCompositionSession(this, pCtx, pRange, pComp);
            RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
            pSession->Release();
            pCtx->Release();
        }
    }
    if (pRange) { pRange->Release(); }
    if (pComp) { pComp->Release(); }
}

void CTextService::AbortCompositionState()
{
    // 先取出并断开成员，避免后续键在坏状态上重入。
    ITfRange* pRange = _compRange;
    ITfComposition* pComp = _pComposition;
    _compRange = nullptr;
    _pComposition = nullptr;
    _pinyin.clear();
    _fSelfInsert = FALSE;
    _pinyinMode = false; // 降级：下次焦点/激活事件重置脏标记后可重新启用
    _modeDirty.store(true);
    ClearCandidates();

    // 关键：必须在宿主中结束悬挂组合，否则 Chromium 会永久停留在组合态，
    // 污染同一 TSF 上下文里的其他输入法（现象：切到微软拼音也无法输入/退格）。
    __try
    {
        AbortHostCompositionInner(pRange, pComp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LX_LOG(L"ime", L"结束悬挂组合再次异常，宿主可能需切换焦点恢复");
        // 引用不再 Release（对象已损坏），交给框架/进程清理；进程内状态已断开。
    }
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
