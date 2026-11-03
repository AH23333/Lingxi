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
#include "../common/punct.h"

#include <functional>
#include <memory>

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

// ---- v 模式（数字大写/计算器）键位支持 ----
// 引擎侧见 ui/engine/calcnum.cpp；触发口径与该处字符集一致：
// 组合 "v"+calc 字符。双拼模式 v=声母(zh)，不触发（调用方把关）。

static bool IsCalcChar(wchar_t c)
{
    return (c >= L'0' && c <= L'9') ||
           c == L'+' || c == L'-' || c == L'*' || c == L'/' ||
           c == L'(' || c == L')' || c == L'.';
}

// calc 键 → 字符（美式键位；Shift 映射 * ( ) +；0=非 calc 键）。
// 数字/加减乘除括号点全走组合，不再翻页/选词/标点（v 模式专属）。
static wchar_t CalcKeyChar(WPARAM wParam)
{
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (wParam >= L'0' && wParam <= L'9')
    {
        if (!shift) return (wchar_t)wParam;
        if (wParam == L'8') return L'*';
        if (wParam == L'9') return L'(';
        if (wParam == L'0') return L')';
        return 0; // Shift+1..7（!@#$%^&）非 calc
    }
    if (!shift)
    {
        if (wParam == VK_OEM_MINUS) return L'-';
        if (wParam == VK_OEM_PERIOD) return L'.';
        if (wParam == VK_OEM_2) return L'/';
        return 0;
    }
    if (wParam == VK_OEM_PLUS) return L'+'; // Shift+= → +
    return 0;
}

// v 模式组合判定：拼音串形如 "v"+calc 字符（触发后粘性，直至组合结束）
static bool CalcCtxActive(const std::wstring& pinyin)
{
    return pinyin.size() >= 2 && pinyin[0] == L'v' && IsCalcChar(pinyin[1]);
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
    bool premature = false; // 判定时前台 TIP 还不是我们：激活/切换过渡窗口
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
        else if (_fActivated)
        {
            premature = true;
        }
    }
    if (py != _pinyinMode)
        LX_LOG(L"ime", L"拼音模式: %d", (int)py); // 仅变化时记录
    _pinyinMode = py;
    // 离开中文 profile：复位临时英文，避免跨应用残留
    if (!py)
    {
        _tempEnglish = false;
    }

    // ---- 激活竞态自愈 ----
    // ActivateEx 只投递一次检测；若那一次落在 "TIP 激活过渡窗口"（前台 TIP 尚未
    // 切到我们），纯 TSF profile 切换又不改线程 HKL（#18），MaybeRedetect 永远
    // 不再触发，_pinyinMode 卡死 false → 拼音静默降级成直通且不自愈。
    // 两级自愈：①快速重检 8×250ms≈2s 覆盖常规激活过渡；②耗尽后降级 1s 慢速
    // 轮询——premature 判定每次只有一次进程内 GetForeground 查询（µs 级，
    // CoCreateInstance 仅在 weForeground 时才执行），前台回归后 ≤1s 自愈进位；
    // 正常场景（py 判定完成）KillTimer 完全静止，不会空转。原实现耗尽后
    // "只打日志不 KillTimer"——250ms 周期定时器泄漏，宿主进程 4/秒 TSF 查询
    // 永久空转（曾刷出 390 行/97 秒日志风暴）。
    if (premature && _hwndMarshal)
    {
        if (_modeRetryLeft > 0)
        {
            _modeRetryLeft--;
            SetTimer(_hwndMarshal, kModeRetryTimer, 250, NULL);
        }
        else if (!_pinyinMode)
        {
            if (!_modeSlowRetry)
            {
                _modeSlowRetry = true;
                LX_LOG(L"ime", L"模式快速重检耗尽，降级 1s 慢速自愈轮询（前台回归后自动进位）");
            }
            SetTimer(_hwndMarshal, kModeRetryTimer, 1000, NULL); // 降级慢速：KeepTimer 自愈
        }
    }
    else
    {
        _modeRetryLeft = 8; // 下一次 RequestModeDetect 重新享有完整重试预算
        _modeSlowRetry = false;
        if (_hwndMarshal) KillTimer(_hwndMarshal, kModeRetryTimer);
    }
}

// ==================== 组合编辑原语 ====================

// ==================== 选词联想（GAP §11 主线一） ====================
// 根因：中文选词上屏 → _fSelfInsert 守卫（原为符号插入设计，PITFALL #24）抑制
// OnEndEdit 上下文预测 → 联想永远不触发。修复=上屏路径显式发起：复用现成的
// "读前文+caret 发无组合 CTX 帧"通道——UI 侧把该帧当普通续写做链预测（LLM 优先，
// n-gram 兜底），候选窗弹出；点其候选=上屏+连发下一轮；空格/数字在联想窗可见
// 时是既有选词键（对标 MS 空格连出）；其他任何无组合按键立即撤销（OnTestKeyDown）。
bool CTextService::AssocEligible() const
{
    // ⚠️ LoadSettings() 每次现读（50ms 级设置页语义，同 _inlineComp 消费点），
    // 用户拨开关无需重启输入法即时生效。
    return _pinyinMode && !_tempEnglish && _fActivated &&
           LoadSettings().assocAfterPick;
}

void CTextService::SendAssocCtx()
{
    if (!AssocEligible()) return;
    {
        EnterCriticalSection(&_cs);   // 防双发：已有联想在途即跳过
        if (_assocReq) { LeaveCriticalSection(&_cs); return; }
        _assocReq = true;
        LeaveCriticalSection(&_cs);
    }
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx))
    {
        EnterCriticalSection(&_cs); _assocReq = false; LeaveCriticalSection(&_cs);
        return;
    }
    ITfContext* pCtxSession = pCtx;
    auto task = [this, pCtxSession](TfEditCookie ec) -> HRESULT
    {
        // inline 组合异常残留（上屏会话未跑完）时读到的位置不可靠：放弃本轮。
        if (_compRange)
        {
            EnterCriticalSection(&_cs); _assocReq = false; LeaveCriticalSection(&_cs);
            return S_OK;
        }
        // 本会话排在提交会话之后（TSF 同 context FIFO）→ 读到的是上屏后的新语境。
        RECT rc{};
        bool hasRect = false;
        ITfContextView* pView = nullptr;
        if (SUCCEEDED(pCtxSession->GetActiveView(&pView)) && pView)
        {
            RECT rcText{};
            if (SUCCEEDED(SafeGetTextExt(pView, pCtxSession, ec, nullptr, &rcText)))
            { rc = rcText; hasRect = true; }
            pView->Release();
        }
        if (!hasRect)
        {
            GUITHREADINFO gti{ sizeof(gti) };
            if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret)
            {
                POINT pt = { gti.rcCaret.left, gti.rcCaret.bottom };
                ClientToScreen(gti.hwndCaret, &pt);
                rc = { pt.x, pt.y, pt.x, pt.y };
                hasRect = true;
            }
        }
        std::wstring pre;
        TF_SELECTION sel{};
        ULONG fetched = 0;
        if (SUCCEEDED(pCtxSession->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
            fetched == 1 && sel.range)
        {
            ITfRange* pBack = nullptr;
            if (SUCCEEDED(sel.range->Clone(&pBack)) && pBack)
            {
                pBack->Collapse(ec, TF_ANCHOR_END);
                LONG moved = 0;
                pBack->ShiftStart(ec, -kCtxMaxChars, &moved, NULL);
                wchar_t buf[kCtxMaxChars + 1] = L"";
                ULONG cch = 0;
                if (SUCCEEDED(pBack->GetText(ec, 0, buf, kCtxMaxChars, &cch)) && cch > 0)
                    pre.assign(buf, cch);
                pBack->Release();
            }
            sel.range->Release();
        }
        EnterCriticalSection(&_cs);
        const bool carry = PinyinActive() || _compRange;   // 期间已开新组合：作废本轮
        if (carry || pre.empty() || !hasRect) _assocReq = false;
        LeaveCriticalSection(&_cs);
        if (carry || pre.empty() || !hasRect)
        {
            LX_LOG(L"ime", L"联想作废 carry=%d preLen=%u rect=%d",
                   (int)carry, (unsigned)pre.size(), (int)hasRect);
            return S_OK;   // 文档头/拿不到位置：不弹（下次上屏再说）
        }
        LX_LOG(L"ime", L"联想发出 preLen=%u caret=(%d,%d)",
               (unsigned)pre.size(), rc.left, rc.top);
        // _assocReq 保持到 CANDS 对号（OnCandidates 认账转 _assocShown）
        SendCtx(pre, rc, CTX_F_ASSOC, L"");
        return S_OK;
    };
    auto* pSession = new CPinyinEditSession(this, pCtx, task);
    RequestSessionEx(pCtx, _tfClientId, pSession, TF_ES_READ);
    pSession->Release();
    pCtx->Release();
}

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
    // M1：会话可能异步执行，而函数尾即释放成员引用——任务必须自持
    // AddRef 快照（旧实现任务读成员，调用方先释放 = UAF 窗口）。快照包
    // 成 RAII：任务正常跑完标记 released 后由 EndComposition 结果路径
    // 收尾；会话没排上（同步+异步双败）任务永不执行时，由最后一个持引
    // 的 lambda 副本析构兜底释放，两条路径都恰好一次。
    struct SnapGuard
    {
        ITfRange* range = nullptr;
        ITfComposition* comp = nullptr;
        bool released = false;
        ~SnapGuard()
        {
            if (!released)
            {
                if (range) range->Release();
                if (comp) comp->Release();
                released = true;
            }
        }
    };
    auto snap = std::make_shared<SnapGuard>();
    snap->range = _compRange;
    if (snap->range) snap->range->AddRef();
    snap->comp = _pComposition;
    if (snap->comp) snap->comp->AddRef();
    auto task = [this, doReplace, replaceText, snap](TfEditCookie ec) -> HRESULT
    {
        HRESULT hr = S_OK;
        LX_LOG(L"ime", L"[end] enter task range=%p comp=%p doReplace=%d",
               (void*)snap->range, (void*)snap->comp, (int)doReplace);
        if (snap->range && doReplace)
        {
            hr = snap->range->SetText(ec, 0, replaceText.c_str(), (LONG)replaceText.size());
            LX_LOG(L"ime", L"[end] SetText=0x%08X", hr);
        }
        if (SUCCEEDED(hr) && snap->comp)
        {
            hr = snap->comp->EndComposition(ec);
            LX_LOG(L"ime", L"[end] EndComposition=0x%08X", hr);
        }
        snap->released = true; // 跑完即定音：析构不再兜底
        return hr;
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

// B1 部分提交：cover 是否指向有效剩余拼音。
// 条件：0 < cover < 串长（部分消耗）且 _pinyin 全字母/隔音符（其余字符的
// 下标映射不可靠，全清兜底）。撇号是引擎 inOfCtx 计入的合法组合字符
// （B6），cover 按原始 context 下标映射，substr 位置正确。是则 rest=剩余串。
bool CTextService::PinyinKeepRest(uint32_t cover, std::wstring& rest)
{
    if (cover == 0 || cover >= _pinyin.size()) return false;
    for (wchar_t ch : _pinyin)
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'\''))
            return false;
    rest = _pinyin.substr(cover);
    return !rest.empty();
}

// 提交汉字。cover（B1）满足部分消耗条件时：上屏词后剩余拼音留在组合里
// 继续输入（无组合模式直接改内存串再预测；内联模式重启组合承载剩余串）。
// 其余情况维持旧行为：提交即全清。
void CTextService::PinyinCommitHanzi(const std::wstring& hanzi, uint32_t cover)
{
    // 先取剩余（PinyinEndComposition 会清 _pinyin，必须在其前判定）
    std::wstring rest;
    bool keep = PinyinKeepRest(cover, rest);
    if (!_inlineComp)
    {
        // 无组合：宿主里没有拼音字母，直接把汉字插到光标处。
        LX_LOG(L"ime", L"无组合上屏汉字 chars=%u cover=%u keep=%u",
               (uint32_t)hanzi.size(), cover, (uint32_t)keep);
        _fSelfInsert = TRUE;
        InsertTextAtSelection(hanzi);
        _fSelfInsert = FALSE;
        if (keep)
        {
            // 部分提交：剩余拼音继续组合（用户长串选短词不再"白打"）
            _pinyin = rest;
            ClearCandidates();
            SendPinyinCtx();
            return;
        }
        PinyinResetNoInline();
        SendAssocCtx();   // 选词联想：全量上屏后主动发联想帧（GAP §11）
        return;
    }
    ITfContext* pCtx = nullptr;
    if (!GetFocusContext(_pThreadMgr, &pCtx))
    {
        LX_LOG(L"ime", L"提交汉字失败：无焦点上下文");
        return;
    }
    LX_LOG(L"ime", L"组合提交汉字 chars=%u cover=%u keep=%u",
           (uint32_t)hanzi.size(), cover, (uint32_t)keep);
    PinyinEndComposition(pCtx, &hanzi);
    pCtx->Release();
    // 内联模式部分提交：重启组合承载剩余拼音（PinyinStart 重置 _pinyin 并重建
    // 组合）。模式过渡期（_fActivated/_pinyinMode 掉线）PinyinStart 会拒启——
    // 重启失败则回退全清收尾，不留悬挂候选。
    if (keep && _fActivated && _pinyinMode)
    {
        PinyinStart(rest[0]);
        for (size_t i = 1; i < rest.size(); i++)
            PinyinAppend(rest[i]);
        if (PinyinActive()) return;
        LX_LOG(L"ime", L"部分提交后重启组合失败，回退全清");
    }
    ClearCandidates();
    SendHide();
    SendAssocCtx();   // inline 全量上屏后同样联想（GAP §11）
}

void CTextService::PinyinCommitRaw()
{
    if (!_inlineComp)
    {
        // 无组合：回车把原始拼音字母作为普通文本上屏。
        if (!_pinyin.empty())
        {
            std::wstring raw = _pinyin;
            // 全角字母数字（搜狗 F4 对标，默认关）：原文上屏整串转全角
            if (LoadSettings().fullWidthAlnum) raw = ToFullWidthStr(raw);
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
        // 读已上屏上文（光标前 ≤kCtxMaxChars）：与 CReadSession 同法。无组合模式
        // （默认）下拼音从未写入宿主，选区即插入点，读到的全是已提交文本——
        // 这是 LLM 整句重排的上下文来源（R3）。
        std::wstring committed;
        TF_SELECTION selCtx{};
        ULONG fetchedCtx = 0;
        if (SUCCEEDED(pCtxSession->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selCtx, &fetchedCtx)) &&
            fetchedCtx == 1 && selCtx.range)
        {
            ITfRange* pBack = nullptr;
            if (SUCCEEDED(selCtx.range->Clone(&pBack)) && pBack)
            {
                pBack->Collapse(ec, TF_ANCHOR_END);
                LONG moved = 0;
                pBack->ShiftStart(ec, -kCtxMaxChars, &moved, NULL);
                wchar_t buf[kCtxMaxChars + 1] = L"";
                ULONG cch = 0;
                if (SUCCEEDED(pBack->GetText(ec, 0, buf, kCtxMaxChars, &cch)) && cch > 0)
                    committed.assign(buf, cch);
                pBack->Release();
            }
            selCtx.range->Release();
        }

        SendCtx(pinyin, rc, CTX_F_PINYIN | CTX_F_CTX_PREFIX, committed);
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
    // 临时英文模式（Shift 单击切换）：字母/标点一律放行，不进拼音
    if (!_pinyinMode || _tempEnglish) return false;
    // Ctrl/Alt 组合键永远直通（应用快捷键：Ctrl+C/V/F、Ctrl+1..9、Alt+字母等）。
    // 此前只看 wParam 字母值，Ctrl+C 在中文模式下被当拼音 'c' 吃掉——用户实测
    // 报告"输入法无法使用快捷键"的根因。Shift/Caps 保留混排语义，不受此闸影响。
    if (CtrlOrAltDown()) return false;
    // 中英混排：Shift 按下或 CapsLock 开启时，字母直通（输入大写/英文）
    bool shiftOrCaps = (GetKeyState(VK_SHIFT) & 0x8000) != 0 || (GetKeyState(VK_CAPITAL) & 1) != 0;
    if (PinyinActive())
    {
        // v 模式（数字大写/计算器）：全拼专属。组合为 "v" 时按 calc 键触发；
        // 进入后 calc 键全吃——数字/加减乘除括号点不再走翻页/选词/标点路径。
        // 双拼 v=声母(zh)，不触发。_pinyin 仅 UI 线程写，锁外读为既定模式。
        if (!LoadSettings().shuangpin && CalcKeyChar(wParam) != 0 &&
            (_pinyin == L"v" || CalcCtxActive(_pinyin)))
            return true;
        if (IsLetterKey(wParam))
        {
            // 输入中的 Shift+字母：也要吃（HandlePinyinKey 会先提交原文再放行）
            return true;
        }
        switch (wParam)
        {
        case VK_BACK: case VK_ESCAPE: case VK_RETURN:
        case VK_SPACE: case VK_TAB:
            return true;
        case VK_PRIOR: case VK_NEXT: // PageUp/PageDown：输入中翻页（吃）
            return CandidatesVisible();
        case VK_OEM_MINUS: case VK_OEM_PLUS: // -/=：Shift 出破折号/＋；无 Shift 多页翻页
            if (shiftOrCaps) return IsChinesePunctKey(wParam);
            {
                bool multi = false;
                EnterCriticalSection(&_cs);
                multi = _cands.size() > kPageSize && _verCands == _verSent;
                LeaveCriticalSection(&_cs);
                return multi;
            }
        case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
        case VK_LEFT: case VK_RIGHT:
            // 组合存续期移动光标类导航键一律归 IME：Up/Down/Home/End 翻页、
            // Left/Right 吞下保守组合（组合光标编辑为 M5 尾巴）。此前放行会让
            // 宿主光标移出组合范围，后续 SetText 与光标错位（B5 VK_SHIFT 同族
            // 缺口）。Ctrl/Alt 组合键已在上方直通闸放行，不受影响。
            return true;
        case VK_OEM_7:
            // B6 隔音符 eat 侧：组合中无 Shift 撇号必须进组合，不能依赖
            // cnPunct 标点映射（cnPunct 关时 IsChinesePunctKey=false，撇号
            // 会穿透宿主，B6 死路）。Shift+'（引号）与双拼维持原标点/放行路径。
            if (!shiftOrCaps && !LoadSettings().shuangpin) return true;
            return IsChinesePunctKey(wParam);
        }
        if (wParam >= L'1' && wParam <= L'9' && !shiftOrCaps)
            return CandidatesVisible(); // 无 Shift 数字 = 当前页选词
        if ((wParam == VK_OEM_4 || wParam == VK_OEM_6) && !shiftOrCaps)
            return CandidatesVisible(); // 以词定字（GAP §1）：候选可见 [ ] 才认领，不可见原【】标点/放行
        return IsChinesePunctKey(wParam); // 其余标点（含 Shift 数字 ！？（）等）
    }
    // 未开始输入：Shift/Caps 下的字母直通，普通字母启动拼音，中文标点转换
    if (shiftOrCaps && IsLetterKey(wParam)) return false;
    if (IsLetterKey(wParam)) return true;
    // 全角字母数字（搜狗 F4 对标，默认关）：非组合数字认领，KeyDown 侧转全角
    // 直出；主行 Shift 数字是符号仍走标点道，不认领。
    if (LoadSettings().fullWidthAlnum &&
        ((wParam >= L'0' && wParam <= L'9' && !shiftOrCaps) ||
         (wParam >= VK_NUMPAD0 && wParam <= VK_NUMPAD9)))
        return true;
    return IsChinesePunctKey(wParam);
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

// 候选 + 该候选消耗的拼音前缀字符数（B1 部分提交）；cover=0 表示未知/全清。
// 单次临界区内同时取词与 cover，杜绝两次读取间新候选回包造成的错位。
bool CTextService::GetCandidateEx(uint32_t index, std::wstring& out, uint32_t& cover)
{
    if (!_csInit) return false;
    EnterCriticalSection(&_cs);
    bool ok = (_verCands == _verSent && index < _cands.size());
    if (ok)
    {
        out = _cands[index];
        cover = index < _candCover.size() ? _candCover[index] : 0;
    }
    LeaveCriticalSection(&_cs);
    return ok;
}

bool CTextService::IsChinesePunctKey(WPARAM wParam)
{
    if (!LoadSettings().cnPunct) return false;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    wchar_t out[3];
    bool dq = false, sq = false;
    return MapChinesePunct(wParam, shift, dq, sq, out) > 0;
}

bool CTextService::CommitChinesePunct(WPARAM wParam)
{
    if (!LoadSettings().cnPunct) return false;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    wchar_t out[3];
    int n = 0;
    if (_csInit)
    {
        EnterCriticalSection(&_cs);
        n = MapChinesePunct(wParam, shift, _cnDqOpen, _cnSqOpen, out);
        LeaveCriticalSection(&_cs);
    }
    if (n <= 0) return false;

    std::wstring punct(out, n);

    // 拼音活动：
    //  无组合模式（默认）：把“首选汉字 + 标点”合并为一次插入，避免两个异步
    //    编辑会话在 Chromium 上出现光标/顺序竞态；无候选时保留字母原文。
    //  内联组合模式：先结束组合上屏汉字，再插标点。
    if (PinyinActive())
    {
        std::wstring hanzi;
        WaitForLatestCandidates(400); // 与空格/数字键同口径：候选在途时兜底
        bool got = GetCandidate(0, hanzi);
        if (got) NotifyPicked(0); // 标点前的首选汉字也作为选字反馈
        if (!_inlineComp)
        {
            std::wstring all = got ? hanzi : _pinyin;
            all += punct;
            _fSelfInsert = TRUE;
            InsertTextAtSelection(all);
            _fSelfInsert = FALSE;
            PinyinResetNoInline();
        }
        else
        {
            if (got) PinyinCommitHanzi(hanzi);
            else     PinyinCommitRaw();
            ClearCandidates();
            SendHide();
            _fSelfInsert = TRUE;
            InsertTextAtSelection(punct);
            _fSelfInsert = FALSE;
        }
        LX_LOG(L"ime", L"中文标点 vk=%u shift=%d -> %u chars (含首选)",
               (uint32_t)wParam, (int)shift, n);
        return true;
    }

    // 非拼音活动：直接上屏标点
    _fSelfInsert = TRUE;
    InsertTextAtSelection(punct);
    _fSelfInsert = FALSE;
    LX_LOG(L"ime", L"中文标点 vk=%u shift=%d -> %u chars",
           (uint32_t)wParam, (int)shift, n);
    return true;
}

void CTextService::HandlePinyinKey(ITfContext* pContext, WPARAM wParam, BOOL* pfEaten)
{
    *pfEaten = TRUE;
    LX_LOG(L"key", L"HandlePinyinKey vk=%u('0x%02X') active=%d pinyinLen=%u",
           (uint32_t)wParam, (uint32_t)wParam, (int)PinyinActive(), (uint32_t)_pinyin.size());

    // 个别应用不走 TestKeyDown 直达这里：Ctrl/Alt 组合键同样必须放行
    // （与 PinyinKeyEaten 的闸保持一致）
    if (CtrlOrAltDown())
    {
        *pfEaten = FALSE;
        return;
    }

    // Shift 本体：纯修饰键，任何状态（空组合/组合中）都不触发提交或选词，原样放行。
    // 修复：组合中 tap Shift 曾落 default 的"unmapped 提交首选"路径，把当前首选
    // 误上屏（用户"按 Shift 想打大写"场景）；Ctrl/Alt 本体在 KeyDown 时 keystate
    // 已置位、被上面 CtrlOrAltDown 覆盖，Shift 无该覆盖，需单列。
    // （weasel 参照：WeaselTSF/KeyEventSink.cpp:189 将 VK_SHIFT 注册为 preserved key
    //  单独处理、不当普通键分发——本处等价语义为分发层放行。）
    if (wParam == VK_SHIFT)
    {
        *pfEaten = FALSE;
        return;
    }

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

    // 非活动态：字母键启动拼音；退格等控制键放行；中文标点转换并上屏；
    // 其余非字母键放行（绝不能吞掉退格，否则已上屏文字无法删除）。
    if (!PinyinActive())
    {
        if (!IsLetterKey(wParam))
        {
            if (wParam == VK_BACK)
                _lastBackspaceTick = GetTickCount64(); // 退格删除：抑制随之而来的补全预测
            // 无组合候选窗选词道（GAP §11 联想 + 英文/续写补全统一入口，用户
            // 2026-11-02 报"英文输入无法数字选词"）：Test 三面门对候选可见的
            // Tab/数字/Esc 一律认领，Down 必须给对称出口，否则键被吞既不上屏
            // 也不选词。空格/Tab=本页首选，1-9=本页第 n 词，Esc=撤销候选窗。
            // 非选词数字（超出候选数）仍放行写正文。联想批的连发下一轮在
            // InsertCandidate 尾按 _assocShown 判定，本道不再特判。
            {
                bool shown = false;
                bool dismiss = (wParam == VK_ESCAPE);
                uint32_t idx = 0;
                bool ok = false;
                EnterCriticalSection(&_cs);
                shown = (_verCands == _verSent && !_cands.empty());
                if (shown)
                {
                    if (wParam == VK_SPACE || wParam == VK_TAB)
                        idx = _pageStart;
                    else if (wParam >= L'1' && wParam <= L'9')
                        idx = _pageStart + (uint32_t)(wParam - L'1');
                    else
                        idx = 0xFFFFFFFF;
                    ok = (idx < (uint32_t)_cands.size());
                }
                LeaveCriticalSection(&_cs);
                if (shown && ok)
                {
                    *pfEaten = TRUE;
                    InsertCandidate(idx);   // 提交+清窗+（联想批）连发下一轮
                    return;
                }
                if (shown && dismiss)
                {
                    *pfEaten = TRUE;
                    ClearCandidates();
                    SendHide();
                    return;
                }
            }
            if (CommitChinesePunct(wParam)) return;    // 中文标点（已上屏，吃掉）
            // 全角字母数字（搜狗 F4 对标，默认关）：非组合数字直出转全角０-９。
            // 主行数字按住 Shift 是符号（已由上方标点道处理），不截；
            // 小键盘受 NumLock 影响（VK_NUMPAD* 仅数字态出现）；Ctrl/Alt 组合
            // 已在函数入口放行，数字选词漏网（超页）数字按全角口径写正文。
            if (LoadSettings().fullWidthAlnum)
            {
                wchar_t dch = 0;
                if (wParam >= L'0' && wParam <= L'9' &&
                    !(GetKeyState(VK_SHIFT) & 0x8000))
                    dch = (wchar_t)wParam;
                else if (wParam >= VK_NUMPAD0 && wParam <= VK_NUMPAD9)
                    dch = (wchar_t)(L'0' + (wParam - VK_NUMPAD0));
                if (dch)
                {
                    std::wstring fw(1, ToFullWidthW(dch));
                    _fSelfInsert = TRUE;
                    InsertTextAtSelection(fw);
                    _fSelfInsert = FALSE;
                    *pfEaten = TRUE;
                    LX_LOG(L"ime", L"全角数字 vk=%u 上屏", (uint32_t)wParam);
                    return;
                }
            }
            *pfEaten = FALSE;
            return;
        }
        wchar_t ch0 = (wchar_t)((wParam >= L'A' && wParam <= L'Z') ? (wParam + 32) : wParam);
        // v 与普通字母一致走 PinyinStart 组合（"v" 非法拼音 → 引擎 0 候选，
        // 候选窗 echo 回显；空格/回车 PinyinCommitRaw 原样上屏 "v"/"vlog"，
        // 退格 PinyinShrink、Esc PinyinCancel、中文标点 CommitChinesePunct
        // 合并上屏 "v，"）。v 临时英文由此成为全拼默认行为，不再依赖
        // "悬账+pfEaten=FALSE 放行"——真机 TSF 分发 Test 已吃 → KeyDown 的
        // FALSE 不保证回传宿主，旧机制字母全被吞（用户报"v 键无输出"根因）。
        PinyinStart(ch0);
        return;
    }

    if (IsLetterKey(wParam))
    {
        wchar_t ch = (wchar_t)((wParam >= L'A' && wParam <= L'Z') ? (wParam + 32) : wParam);
        PinyinAppend(ch);
        return;
    }
    // 拼音隔音符（B6）：组合中无 Shift 的撇号（美式 VK_OEM_7 = '）进组合当
    // 切分边界，引擎按 xi'an 归一消歧（西安 vs 先）。Shift+'（引号）、
    // 双拼（键位映射滤撇号，隔音无组合意义）、空组合态不进组合，
    // 维持原 unmapped 提交/放行行为。
    if (wParam == VK_OEM_7 && PinyinActive() && !shiftOrCaps && !LoadSettings().shuangpin)
    {
        PinyinAppend(L'\'');
        return;
    }
    // v 模式（数字大写/计算器）：全拼专属。组合为 "v" 时按 calc 键触发追加
    // 进入 calc 态；进入后 calc 键（数字/运算符/点）持续追加，不选词/翻页。
    // 误入（"va"）自动脱离 calc 口径；退格删回 "v" 可再触发。空格/回车仍走
    // 下方 switch：候选 cover=整串（引擎侧拦截），空格上屏首选结果，回车
    // 原样上屏表达式。
    if (!LoadSettings().shuangpin)
    {
        wchar_t cc = CalcKeyChar(wParam);
        if (cc != 0 && (_pinyin == L"v" || CalcCtxActive(_pinyin)))
        {
            PinyinAppend(cc);
            return;
        }
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
    // 翻页：PageDown 下一页、PageUp 上一页；=/− 无 Shift 翻页、Shift 出 ＋/——
    case VK_NEXT:
        PageCandidates(+1);
        return;
    case VK_PRIOR:
        PageCandidates(-1);
        return;
    case VK_OEM_PLUS:
        if (shiftOrCaps && CommitChinesePunct(wParam)) return;
        PageCandidates(+1);
        return;
    case VK_OEM_MINUS:
        if (shiftOrCaps && CommitChinesePunct(wParam)) return;
        PageCandidates(-1);
        return;
    // 导航键翻页（B7 配套）：Up/Down 上下翻页、Home/End 首末页。PinyinKeyEaten
    // 已在组合期吃下这些键（防宿主光标移出组合范围），此处给出有用语义；
    // 无候选/已在目标页时为无操作。
    case VK_UP:
        PageCandidates(-1);
        return;
    case VK_DOWN:
        PageCandidates(+1);
        return;
    case VK_HOME:
        PageCandidatesHomeEnd(false);
        return;
    case VK_END:
        PageCandidatesHomeEnd(true);
        return;
    case VK_LEFT:
    case VK_RIGHT:
        // 吞下保组合：组合光标编辑为 M5 尾巴（预留），放行会造成光标错位。
        return;
    case VK_SPACE:
    case VK_TAB:
    {
        // 拼音串已发出、候选可能还在路上（引擎 µs 级 + 管道往返数毫秒）。
        // 快打时字母上屏后极短时间按空格，候选尚未回包 —— 有界等待一次，
        // 避免"该出汉字的空格却把字母原样提交"。超时则维持原回退语义。
        // 预算 80→400ms：实测选字后 Learn 触发的磁盘写盘曾把候选响应拖到
        // 464ms（现已在引擎侧后台化），重载机器上 Predict 本身可达 150ms+，
        // 80ms 预算会被击穿 —— 候选就绪时该等待零开销，仅 pending 时兜底。
        WaitForLatestCandidates(400);
        std::wstring hanzi;
        uint32_t cover = 0;
        if (GetCandidateEx(_pageStart, hanzi, cover)) { NotifyPicked(_pageStart); PinyinCommitHanzi(hanzi, cover); }
        else                                           PinyinCommitRaw();
        return;
    }
    case VK_OEM_4:
    case VK_OEM_6:
    {
        // 以词定字（GAP §1 对齐微软）：组合中无 Shift [ 取本页首选词首字、
        // ] 取末字，单字上屏即组合收束（B1 cover 部分消耗语义同数字键；
        // emoji 融合候选首尾可能是代理对，取完整码点）。候选不可见或
        // Shift 态回退原【】｛｝标点路径。不 NotifyPicked：取字≠选词，
        // 记整词学习会污染（fengzheng[→风 不该喂"风筝"），与微软
        // "以词定字不计数"口径一致。
        if (!shiftOrCaps && CandidatesVisible())
        {
            std::wstring hanzi;
            uint32_t cover = 0;
            WaitForLatestCandidates(400); // 与空格/数字键同口径：候选在途时兜底
            if (GetCandidateEx(_pageStart, hanzi, cover) && !hanzi.empty())
            {
                size_t len = 1;
                if (wParam == VK_OEM_4)
                {
                    if (hanzi.size() >= 2 && (hanzi[0] & 0xFC00) == 0xD800) len = 2;
                }
                else if (hanzi.size() >= 2 && (hanzi.back() & 0xFC00) == 0xDC00)
                {
                    len = 2;
                }
                PinyinCommitHanzi(hanzi.substr(wParam == VK_OEM_4 ? 0 : hanzi.size() - len, len), cover);
                return;
            }
        }
        if (CommitChinesePunct(wParam)) return; // 【】｛｝：原标点语义保持
        return; // cnPunct 关且候选消失（Test/Down 竞态窗）：吞下无操作（同翻页空页口径）
    }
    default:
        if (wParam >= L'1' && wParam <= L'9' && !shiftOrCaps)
        {
            std::wstring hanzi;
            uint32_t cover = 0;
            uint32_t idx = _pageStart + (uint32_t)(wParam - L'1');
            WaitForLatestCandidates(400); // 与空格同口径：候选在途时有界等待
            if (GetCandidateEx(idx, hanzi, cover)) { NotifyPicked(idx); PinyinCommitHanzi(hanzi, cover); }
            else                                   PinyinCommitRaw();
            return;
        }
        // 中文标点（。，？！（）等）：提交首选汉字后上屏标点
        if (CommitChinesePunct(wParam)) return;
        // Unmapped other keys: commit the first-choice Chinese character (the original key is subsequently released); if no candidate, keep the original letters
        {
            std::wstring hanzi;
            uint32_t cover = 0;
            WaitForLatestCandidates(400); // 与空格/数字键同口径：候选在途时兜底
            if (GetCandidateEx(0, hanzi, cover)) { NotifyPicked(0); PinyinCommitHanzi(hanzi, cover); }
            else                                  PinyinCommitRaw();
            // 部分提交后组合仍在（PinyinCommitHanzi 内已发起下一轮预测），
            // 不清候选/不藏窗，避免与在途候选回包竞态；全清时维持原收尾。
            if (!PinyinActive())
            {
                ClearCandidates();
                SendHide();
            }
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
