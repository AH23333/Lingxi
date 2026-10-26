#include "service.h"
#include "editsession.h"

#include "../common/proto.h"
#include "../common/guids.h"
#include "../common/log.h"

extern LONG g_cComponents;

namespace lingxi {

static const wchar_t kMarshalWndClass[] = L"LingxiImeMarshalWnd";

// 编辑会话请求：同步优先（transitory 上下文只支持同步），被拒则降级异步。
static HRESULT RequestSession(ITfContext* pCtx, TfClientId cid, ITfEditSession* pSes, DWORD baseFlags)
{
    HRESULT hrSession = E_FAIL;
    HRESULT hr = pCtx->RequestEditSession(cid, pSes, baseFlags | TF_ES_SYNC, &hrSession);
    if (hr == TS_E_SYNCHRONOUS || hrSession == TS_E_SYNCHRONOUS)
    {
        pCtx->RequestEditSession(cid, pSes, baseFlags | TF_ES_ASYNCDONTCARE, &hrSession);
    }
    return SUCCEEDED(hr) ? hrSession : hr;
}

CTextService::CTextService()
{
    InterlockedIncrement(&g_cComponents);
}

CTextService::~CTextService()
{
    InterlockedDecrement(&g_cComponents);
}

// ==================== IUnknown ====================

STDMETHODIMP CTextService::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx))
        *ppv = (ITfTextInputProcessorEx*)this;
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
        *ppv = (ITfThreadMgrEventSink*)this;
    else if (IsEqualIID(riid, IID_ITfTextEditSink))
        *ppv = (ITfTextEditSink*)this;
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
        *ppv = (ITfKeyEventSink*)this;
    else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
        *ppv = (ITfThreadFocusSink*)this;
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
        *ppv = (ITfCompositionSink*)this;
    if (*ppv) { AddRef(); return S_OK; }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CTextService::AddRef() { return ++_cRef; }

STDMETHODIMP_(ULONG) CTextService::Release()
{
    LONG c = --_cRef;
    if (c == 0) delete this;
    return c;
}

// ==================== 激活 / 停用 ====================

STDMETHODIMP CTextService::Activate(ITfThreadMgr* pThreadMgr, TfClientId tid)
{
    return ActivateEx(pThreadMgr, tid, 0);
}

STDMETHODIMP CTextService::ActivateEx(ITfThreadMgr* pThreadMgr, TfClientId tid, DWORD dwFlags)
{
    LX_LOG(L"ime", L"ActivateEx tid=%u", tid);
    if (!pThreadMgr) return E_INVALIDARG;

    _pThreadMgr = pThreadMgr;
    _pThreadMgr->AddRef();
    _tfClientId = tid;

    if (!_csInit) { InitializeCriticalSection(&_cs); _csInit = true; }

    if (!InitSinks())
        goto ExitError;

    {
        // 若激活时已有焦点文档，立即挂 edit sink
        ITfDocumentMgr* pDoc = nullptr;
        if (SUCCEEDED(_pThreadMgr->GetFocus(&pDoc)) && pDoc)
        {
            InitTextEditSink(pDoc);
            pDoc->Release();
        }
    }

    // 隐藏封送窗口（宿主 UI 线程）
    {
        static bool s_classRegistered = false;
        if (!s_classRegistered)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = MarshalWndProc;
            wc.hInstance = g_hInst;
            wc.lpszClassName = kMarshalWndClass;
            RegisterClassExW(&wc); // 重复注册失败无妨
            s_classRegistered = true;
        }
        _hwndMarshal = CreateWindowExW(0, kMarshalWndClass, L"", WS_POPUP,
                                       0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInst, NULL);
        if (_hwndMarshal)
            SetWindowLongPtrW(_hwndMarshal, GWLP_USERDATA, (LONG_PTR)this);
    }

    _fActivated = true;
    // 模式轮询线程：OnActivated 只置脏标记，检测在独立 MTA 线程执行
    //（脱离宿主消息泵，避开与 CTF 激活处理的重入崩溃——默认关闭，
    //  LINGXI_PINYIN=1 启用拼音模式）
    if (!_modeStopEv)
    {
        _modeStopEv = CreateEventW(NULL, TRUE, FALSE, NULL);
        _modeThread = CreateThread(NULL, 0, s_ModeThreadProc, this, 0, NULL);
    }
    _pipe.Start(this, _hwndMarshal);
    _pipe.Send(MSG_ACTIVATE, NULL, 0);
    return S_OK;

ExitError:
    Deactivate();
    return E_FAIL;
}

STDMETHODIMP CTextService::Deactivate()
{
    LX_LOG(L"ime", L"Deactivate");
    _fActivated = false;
    _pinyinMode = false;
    // 组合若还开着：放弃跟踪（框架会发 OnCompositionTerminated 兜底清理）
    if (_compRange) { _compRange->Release(); _compRange = nullptr; }
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    _pinyin.clear();
    if (_modeStopEv)
    {
        SetEvent(_modeStopEv);
        if (_modeThread)
        {
            WaitForSingleObject(_modeThread, 2000);
            CloseHandle(_modeThread);
            _modeThread = NULL;
        }
        CloseHandle(_modeStopEv);
        _modeStopEv = NULL;
    }
    _pipe.Send(MSG_DEACTIVATE, NULL, 0);
    _pipe.Stop();
    ClearCandidates();
    if (_hwndMarshal)
    {
        SetWindowLongPtrW(_hwndMarshal, GWLP_USERDATA, 0);
        DestroyWindow(_hwndMarshal);
        _hwndMarshal = nullptr;
    }
    UninitSinks();
    if (_pThreadMgr)
    {
        _pThreadMgr->Release();
        _pThreadMgr = nullptr;
    }
    return S_OK;
}

// ==================== Sink 管理 ====================

bool CTextService::InitSinks()
{
    ITfSource* pSrc = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSrc))) return false;
    HRESULT hr1 = pSrc->AdviseSink(IID_ITfThreadMgrEventSink, (ITfThreadMgrEventSink*)this, &_tmSinkCookie);
    HRESULT hr2 = pSrc->AdviseSink(IID_ITfThreadFocusSink, (ITfThreadFocusSink*)this, &_tfSinkCookie);
    // profile 激活通知：同 TIP 内 en-US/zh-CN profile 切换不会重走 Activate。
    // ⚠️ 仅在 LINGXI_PINYIN=1 时注册：与个别宿主的启动崩溃相关（见 HANDOFF 待查）
    wchar_t pyflag[8] = L"";
    bool pinyinEnabled = GetEnvironmentVariableW(L"LINGXI_PINYIN", pyflag, 8) > 0 && pyflag[0] == L'1';
    HRESULT hr4 = S_OK;
    if (pinyinEnabled)
        hr4 = pSrc->AdviseSink(IID_ITfInputProcessorProfileActivationSink,
                               (ITfInputProcessorProfileActivationSink*)this, &_asSinkCookie);
    else
        hr4 = S_OK;
    pSrc->Release();

    ITfKeystrokeMgr* pKm = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm))) return false;
    // fForeground=TRUE：直通+预测形态要求激活期间看到全部按键；
    // Deactivate 时本 sink 会被解除注册，不会影响其他输入法。
    HRESULT hr3 = pKm->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this, TRUE);
    pKm->Release();

    LX_LOG(L"ime", L"InitSinks: tmgr=0x%08X tfocus=0x%08X key=0x%08X act=0x%08X", hr1, hr2, hr3, hr4);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3) && SUCCEEDED(hr4);
}

void CTextService::UninitSinks()
{
    if (_pThreadMgr)
    {
        ITfSource* pSrc = nullptr;
        if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSrc)))
        {
            if (_tmSinkCookie != TF_INVALID_COOKIE) pSrc->UnadviseSink(_tmSinkCookie);
            if (_tfSinkCookie != TF_INVALID_COOKIE) pSrc->UnadviseSink(_tfSinkCookie);
            if (_asSinkCookie != TF_INVALID_COOKIE) pSrc->UnadviseSink(_asSinkCookie);
            pSrc->Release();
        }
        ITfKeystrokeMgr* pKm = nullptr;
        if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm)))
        {
            pKm->UnadviseKeyEventSink(_tfClientId);
            pKm->Release();
        }
    }
    _tmSinkCookie = TF_INVALID_COOKIE;
    _tfSinkCookie = TF_INVALID_COOKIE;
    _asSinkCookie = TF_INVALID_COOKIE;
    InitTextEditSink(nullptr);
}

// ==================== ITfInputProcessorProfileActivationSink ====================

STDMETHODIMP CTextService::OnActivated(DWORD dwProfileType, LANGID langid, REFCLSID rclsid,
                                       REFGUID catid, REFGUID guidProfile, HKL hkl, DWORD dwFlags)
{
    // 回调内零动作：只标记"模式需要重判"，实际检测由管道线程执行
    //（脱离消息泵，避免与 CTF 激活处理重入——见 PITFALLS 启动崩溃条目）
    if (IsEqualCLSID(rclsid, CLSID_LingxiTextService))
        _modeDirty.store(true);
    return S_OK;
}

bool CTextService::InitTextEditSink(ITfDocumentMgr* pDocMgr)
{
    if (_pEditSinkCtx)
    {
        ITfSource* pSrc = nullptr;
        if (SUCCEEDED(_pEditSinkCtx->QueryInterface(IID_ITfSource, (void**)&pSrc)))
        {
            pSrc->UnadviseSink(_editSinkCookie);
            pSrc->Release();
        }
        _pEditSinkCtx->Release();
        _pEditSinkCtx = nullptr;
        _editSinkCookie = TF_INVALID_COOKIE;
    }
    if (!pDocMgr) return true;

    ITfContext* pCtx = nullptr;
    if (FAILED(pDocMgr->GetTop(&pCtx)) || !pCtx) return true;

    ITfSource* pSrc = nullptr;
    if (SUCCEEDED(pCtx->QueryInterface(IID_ITfSource, (void**)&pSrc)))
    {
        pSrc->AdviseSink(IID_ITfTextEditSink, (ITfTextEditSink*)this, &_editSinkCookie);
        pSrc->Release();
    }
    _pEditSinkCtx = pCtx; // GetTop 的引用由我们持有
    LX_LOG(L"ime", L"TextEditSink 挂载 ctx=%p", (void*)pCtx);
    return true;
}

// ==================== ITfThreadMgrEventSink ====================

STDMETHODIMP CTextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP CTextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP CTextService::OnPushContext(ITfContext*) { return S_OK; }
STDMETHODIMP CTextService::OnPopContext(ITfContext*) { return S_OK; }

STDMETHODIMP CTextService::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus)
{
    // 注意参数顺序：SDK 中第一参数是"获得焦点"的文档，第二参数是"失去焦点"的。
    // 挂错方向会导致焦点切换后编辑 sink 停留在旧文档上，预测从此失灵。
    LX_LOG(L"ime", L"OnSetFocus got=%p prev=%p", pdimFocus, pdimPrevFocus);
    InitTextEditSink(pdimFocus);
    if (pdimFocus)
    {
        ITfContext* pCtx = nullptr;
        if (SUCCEEDED(pdimFocus->GetTop(&pCtx)) && pCtx)
        {
            RequestContextRead(pCtx); // 切换焦点立即送一次上下文（续写候选可即时出现）
            pCtx->Release();
        }
    }
    else
    {
        SendHide();
    }
    return S_OK;
}

// ==================== ITfThreadFocusSink ====================

STDMETHODIMP CTextService::OnSetThreadFocus() { return S_OK; }

STDMETHODIMP CTextService::OnKillThreadFocus()
{
    SendHide();
    ClearCandidates();
    return S_OK;
}

// ==================== ITfTextEditSink ====================

STDMETHODIMP CTextService::OnEndEdit(ITfContext* pContext, TfEditCookie ecReadOnly,
                                     ITfEditRecord* pEditRecord)
{
    LX_LOG(L"ime", L"OnEndEdit ctx=%p self=%d", (void*)pContext, (int)_fSelfInsert);
    if (_fSelfInsert)
        return S_OK; // 自己上屏引发的编辑，避免自激发
    if (!_fActivated)
        return S_OK;
    if (_pComposition)
    {
        // 组合期间的外部编辑：重发拼音组合串（保持候选与光标同步）
        SendPinyinCtx();
        return S_OK;
    }
    RequestContextRead(pContext);
    return S_OK;
}

// ==================== 上下文读取与发送 ====================

void CTextService::RequestContextRead(ITfContext* pContext)
{
    // 不再检查 _fForeground：edit sink 只挂在焦点文档上，此处的编辑即当前输入位置；
    // 无窗口焦点的 TSF 宿主（自动化测试等）也能正常工作。
    if (!_fActivated || !pContext) return;
    CReadSession* pSession = new CReadSession(this, pContext);
    RequestSession(pContext, _tfClientId, pSession, TF_ES_READ);
    pSession->Release();
}

void CTextService::SendCtx(const std::wstring& text, const RECT& rc, uint8_t flags)
{
    if (!_fActivated) return;
    if (text.empty())
    {
        SendHide();
        return;
    }

    uint32_t ver = 0;
    EnterCriticalSection(&_cs);
    ver = ++_verSent;
    LeaveCriticalSection(&_cs);

    MsgCtx m{};
    m.ver = ver;
    m.caretX = rc.left;
    m.caretY = rc.top;
    m.caretW = rc.right - rc.left;
    m.caretH = rc.bottom - rc.top;
    m.flags = flags;
    m.textChars = (uint32_t)text.size();
    _pipe.SendStr(MSG_CTX, &m, sizeof(m), text);
    LX_LOG(L"ime", L"SendCtx ver=%u chars=%u flags=%u rc=(%ld,%ld,%ld,%ld)",
           ver, m.textChars, flags, rc.left, rc.top, rc.right, rc.bottom);
}

void CTextService::OnContextRead(const std::wstring& text, const RECT& rc)
{
    if (!_fActivated) return;
    if (text.empty())
    {
        // 空上下文：仅通知 UI 隐藏，不占版本号
        SendHide();
        return;
    }
    SendCtx(text, rc, 0);
}

// ==================== 候选与上屏 ====================

void CTextService::OnCandidates(uint32_t ver, uint32_t replaceLen, std::vector<std::wstring>&& cands)
{
    LX_LOG(L"ime", L"收到候选 ver=%u replaceLen=%u count=%u", ver, replaceLen, (uint32_t)cands.size());
    EnterCriticalSection(&_cs);
    if (ver == _verSent)
    {
        _cands = std::move(cands);
        _verCands = ver;
        _replaceLen = replaceLen;
    }
    LeaveCriticalSection(&_cs);
}

void CTextService::OnPickRequested(uint32_t ver, uint32_t index)
{
    if (_hwndMarshal)
        PostMessageW(_hwndMarshal, WM_APP_LX_PICK, index, ver);
}

void CTextService::HandlePickFromUI(uint32_t ver, uint32_t index)
{
    // 拼音组合中：鼠标选词 = 提交该汉字进组合
    if (_pComposition)
    {
        std::wstring hanzi;
        if (GetCandidate(index, hanzi)) PinyinCommitHanzi(hanzi);
        return;
    }
    InsertCandidate(index); // InsertCandidate 内部会校验版本
}

bool CTextService::CandidatesVisible()
{
    if (!_csInit) return false;
    EnterCriticalSection(&_cs);
    bool visible = !_cands.empty() && _verCands == _verSent;
    LeaveCriticalSection(&_cs);
    return visible;
}

void CTextService::ClearCandidates()
{
    if (!_csInit) return;
    EnterCriticalSection(&_cs);
    _cands.clear();
    _verCands = 0;
    _replaceLen = 0;
    LeaveCriticalSection(&_cs);
}

void CTextService::SendHide()
{
    _pipe.Send(MSG_HIDE, NULL, 0);
}

void CTextService::InsertCandidate(uint32_t index)
{
    std::wstring text;
    {
        EnterCriticalSection(&_cs);
        if (_verCands != _verSent || index >= _cands.size())
        {
            LeaveCriticalSection(&_cs);
            return;
        }
        text = _cands[index];
        LeaveCriticalSection(&_cs);
    }

    LX_LOG(L"ime", L"上屏候选[%u] chars=%u", index, (uint32_t)text.size());
    // ASYNCDONTCARE 在宿主 UI 线程的按键路径上实际为同步执行；
    // _fSelfInsert 覆盖整个调用窗口以过滤自激发的 OnEndEdit。
    _fSelfInsert = TRUE;
    bool ok = false;
    {
        EnterCriticalSection(&_cs);
        uint32_t replaceLen = _replaceLen;
        LeaveCriticalSection(&_cs);
        ok = replaceLen > 0
                 ? ReplaceTextBeforeCaret(text, replaceLen)
                 : InsertTextAtSelection(text);
    }
    _fSelfInsert = FALSE;

    ClearCandidates();
    if (ok) SendHide();
}

bool CTextService::InsertTextAtSelection(const std::wstring& text)
{
    if (!_pThreadMgr) return false;
    ITfDocumentMgr* pDoc = nullptr;
    if (FAILED(_pThreadMgr->GetFocus(&pDoc)) || !pDoc) return false;
    ITfContext* pCtx = nullptr;
    HRESULT hr = pDoc->GetTop(&pCtx);
    pDoc->Release();
    if (FAILED(hr) || !pCtx) return false;

    CInsertSession* pSession = new CInsertSession(this, pCtx, text);
    HRESULT hrSession = RequestSession(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    return SUCCEEDED(hrSession);
}

bool CTextService::ReplaceTextBeforeCaret(const std::wstring& text, uint32_t deleteLen)
{
    if (!_pThreadMgr) return false;
    ITfDocumentMgr* pDoc = nullptr;
    if (FAILED(_pThreadMgr->GetFocus(&pDoc)) || !pDoc) return false;
    ITfContext* pCtx = nullptr;
    HRESULT hr = pDoc->GetTop(&pCtx);
    pDoc->Release();
    if (FAILED(hr) || !pCtx) return false;

    CReplaceSession* pSession = new CReplaceSession(this, pCtx, text, deleteLen);
    HRESULT hrSession = RequestSession(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    return SUCCEEDED(hrSession);
}

// ==================== ITfKeyEventSink ====================

static bool IsSelectionKey(WPARAM wParam)
{
    return wParam == VK_TAB ||
           (wParam >= L'1' && wParam <= L'9') ||
           wParam == VK_ESCAPE;
}

STDMETHODIMP CTextService::OnSetFocus(BOOL fForeground)
{
    _fForeground = fForeground;
    LX_LOG(L"ime", L"KeySink OnSetFocus foreground=%d", fForeground);
    return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
{
    // 拼音模式优先（字母进组合、组合态拦截编辑键）
    if (PinyinKeyEaten(wParam))
    {
        *pfEaten = TRUE;
        return S_OK;
    }
    // 直通形态：候选可见时拦截确认键，其余一律放行
    bool visible = CandidatesVisible();
    if (visible) // 逐键日志只记录候选交互期，避免真实使用时刷屏
        LX_LOG(L"key", L"OnTestKeyDown vk=%u visible=1", (uint32_t)wParam);
    *pfEaten = (visible && IsSelectionKey(wParam)) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
{
    if (_pinyinMode)
    {
        HandlePinyinKey(pContext, wParam, pfEaten);
        return S_OK;
    }
    *pfEaten = FALSE;
    if (!CandidatesVisible()) return S_OK;

    if (wParam == VK_ESCAPE)
    {
        SendHide();
        ClearCandidates();
        *pfEaten = TRUE;
    }
    else if (wParam == VK_TAB)
    {
        InsertCandidate(0);
        *pfEaten = TRUE;
    }
    else if (wParam >= L'1' && wParam <= L'9')
    {
        InsertCandidate((uint32_t)(wParam - L'1'));
        *pfEaten = TRUE;
    }
    return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten)
{
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten)
{
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten)
{
    *pfEaten = FALSE;
    return S_OK;
}

// ==================== 封送窗口 ====================

LRESULT CALLBACK CTextService::MarshalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_APP_LX_PICK)
    {
        auto* svc = (CTextService*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (svc)
            svc->HandlePickFromUI((uint32_t)lParam, (uint32_t)wParam); // ver, index
        return 0;
    }
    if (msg == WM_APP_LX_MODE)
    {
        auto* svc = (CTextService*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (svc)
            lingxi::DetectPinyinModeSEH(svc); // 延迟模式检测（避开激活重入窗口）
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace lingxi
