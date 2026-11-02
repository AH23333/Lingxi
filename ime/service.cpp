#include "service.h"
#include "editsession.h"

#include "../common/proto.h"
#include "../common/guids.h"
#include "../common/log.h"
#include "../common/settings.h"

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

// 兜底插入：TSF 焦点文档不可用时（TSF-unaware 应用的 CUAS 线程焦点关联会被
// 面板搜索态的跨进程前台往返打断，实测 GetFocus NULL 而 Win32 焦点完好），
// 退回本线程 Win32 焦点窗口直接送字符——EDIT/RICHEDIT 用 EM_REPLACESEL，
// 其余窗口逐字符 WM_CHAR（等价正常键盘字符路径）。运行在本线程（封送窗口
// WndProc / 定时器），GetFocus() 即目标线程焦点。
static bool InsertViaWin32Focus(const std::wstring& text)
{
    HWND hFocus = GetFocus();
    if (!hFocus)
    {
        // 本线程无 Win32 焦点（目标应用未复原，或消息落进后台实例）——
        // 带 tid 供日志比对归属（PID 在行前缀）
        LX_LOG(L"ime", L"Win32 兜底：本线程无焦点 fg=%p tid=%u",
               (void*)GetForegroundWindow(), GetCurrentThreadId());
        return false;
    }
    wchar_t cls[16] = {};
    GetClassNameW(hFocus, cls, 16);
    if (_wcsicmp(cls, L"Edit") == 0 || _wcsnicmp(cls, L"RichEd", 6) == 0)
    {
        SendMessageW(hFocus, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
        return true;
    }
    for (wchar_t ch : text)
        SendMessageW(hFocus, WM_CHAR, (WPARAM)ch, 0);
    return true;
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
    if (!_hCandEvt) _hCandEvt = CreateEventW(NULL, FALSE, FALSE, NULL); // 自动重置

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
        // L2：一次初始化原语替代裸 bool——多线程同时激活时旧标志无同步
        // 保证（另一线程可见 flag=true 而注册尚未完成，或两线程同时
        // double-register 后写回交错）。INIT_ONCE 恰一次且阻塞后来者
        // 直到注册落定。
        static INIT_ONCE s_io = INIT_ONCE_STATIC_INIT;
        InitOnceExecuteOnce(&s_io, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = MarshalWndProc;
            wc.hInstance = g_hInst;
            wc.lpszClassName = kMarshalWndClass;
            RegisterClassExW(&wc); // 重复注册失败无妨
            return TRUE;
        }, NULL, NULL);
        _hwndMarshal = CreateWindowExW(0, kMarshalWndClass, L"", WS_POPUP,
                                       0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInst, NULL);
        if (_hwndMarshal)
            SetWindowLongPtrW(_hwndMarshal, GWLP_USERDATA, (LONG_PTR)this);
    }

    _fActivated = true;
    // 拼音模式：投递封送消息，在消息泵空闲点安全检测（绝不碰 CTF 回调窗口内）
    RequestModeDetect();
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
    _modeDirty.store(false);
    // 组合若还开着：放弃跟踪（框架会发 OnCompositionTerminated 兜底清理）
    if (_compRange) { _compRange->Release(); _compRange = nullptr; }
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    _pinyin.clear();
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
    // profile/语言切换的重判：置脏 + 投递封送消息，在消息泵空闲点安全检测。
    // 判据用 GetActiveLanguageProfile（真实激活档案）——纯 TSF TIP 切换 profile
    // 时线程 HKL 不变，不能用 GetKeyboardLayout 判语言（见 PITFALLS #17/#18）。
    pSrc->Release();

    ITfKeystrokeMgr* pKm = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm))) return false;
    // fForeground=TRUE：直通+预测形态要求激活期间看到全部按键；
    // Deactivate 时本 sink 会被解除注册，不会影响其他输入法。
    HRESULT hr3 = pKm->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this, TRUE);
    pKm->Release();

    LX_LOG(L"ime", L"InitSinks: tmgr=0x%08X tfocus=0x%08X key=0x%08X", hr1, hr2, hr3);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3);
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
    InitTextEditSink(nullptr);
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
    RequestModeDetect(); // 焦点变化后重判拼音/直通模式（消息泵空闲点执行）
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
        // M4：失焦清候选——与 OnKillThreadFocus 同口径。不清则 DLL 仍认领
        // 选词键（数字/方向键），失焦下按键被吞不进宿主。
        ClearCandidates();
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
    // Chromium 异步执行自插入会话：OnEndEdit 在 _fSelfInsert 复位后才到达，
    // 用时间戳兜底，避免上屏后误弹上下文预测。
    if (_lastSelfInsertTick != 0 && GetTickCount64() - _lastSelfInsertTick < 400)
        return S_OK;
    // 用户刚退格删除：删除过程中不弹上下文补全（中文输入法删除时不应弹候选）。
    if (_lastBackspaceTick != 0 && GetTickCount64() - _lastBackspaceTick < 500)
        return S_OK;
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

void CTextService::SendCtx(const std::wstring& text, const RECT& rc, uint8_t flags,
                           const std::wstring& ctxPrefix)
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
    // 非联想帧发出 = 旧联想请求作废（QA 2026-11-02）：ASSOC 回包被 UI 队列
    // 合并吞掉时 _assocReq 永挂，随后续写/拼音帧认账会误标 _assocShown——
    // 表现为 ghost 续写窗空格误提交首选而非输入空格。新帧起，联想身份清零。
    if (!(flags & CTX_F_ASSOC)) _assocReq = false;
    LeaveCriticalSection(&_cs);

    MsgCtx m{};
    m.ver = ver;
    m.caretX = rc.left;
    m.caretY = rc.top;
    m.caretW = rc.right - rc.left;
    m.caretH = rc.bottom - rc.top;
    m.ctxChars = (uint32_t)ctxPrefix.size();
    m.textChars = (uint32_t)text.size();
    if (!ctxPrefix.empty()) flags |= CTX_F_CTX_PREFIX;
    m.flags = flags;
    // 帧体 = POD + [可选][u32 len][ctx wchar...] + [u32 len][text wchar...]。
    // 不用 SendStr：ctxPrefix 存在时需要两个字符串段，手工拼装
    // （接收端按长度自洽判别新旧格式，见 pipeserver HandleCtx）。
    std::vector<uint8_t> body;
    AppendPod(body, m);
    if (!ctxPrefix.empty()) AppendStr(body, ctxPrefix);
    AppendStr(body, text);
    _pipe.Send(MSG_CTX, body.data(), (uint32_t)body.size());
    LX_LOG(L"ime", L"SendCtx ver=%u chars=%u ctxChars=%u flags=%u rc=(%ld,%ld,%ld,%ld)",
           ver, m.textChars, m.ctxChars, flags, rc.left, rc.top, rc.right, rc.bottom);
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

void CTextService::OnCandidates(uint32_t ver, uint32_t replaceLen, std::vector<std::wstring>&& cands,
                                std::vector<uint32_t>&& cover)
{
    LX_LOG(L"ime", L"收到候选 ver=%u replaceLen=%u count=%u cover=%u",
           ver, replaceLen, (uint32_t)cands.size(),
           cover.size() == cands.size() ? (uint32_t)cover.size() : 0);
    // cover 平行性校验：不平行（旧 UI 或异常帧）按全 0 处理（提交后全清=旧行为）
    if (cover.size() != cands.size())
        cover.assign(cands.size(), 0);
    bool latest = false;
    EnterCriticalSection(&_cs);
    if (ver == _verSent)
    {
        // 同版本二次帧（StartAsyncRerank 重排写回）= 同一批候选的重排，不是
        // 新批——保持 _pageStart，否则用户翻页中弹回页 1（"翻页乱"）。新批
        // （_verCands != ver，首帧）才归零回首页。
        bool rerank = (_verCands == ver);
        _cands = std::move(cands);
        _candCover = std::move(cover);
        _verCands = ver;
        _assocShown = _assocReq;   // 只有联想请求的回包进入联想态（GAP §11）
        _assocReq = false;      // 联想周期闭合（点选连发另行判定 _assocShown）
        _replaceLen = replaceLen;
        if (!rerank) _pageStart = 0; // 新一批候选回到第一页
        latest = true;
    }
    LeaveCriticalSection(&_cs);
    // 最新版本的候选已入缓存 → 唤醒提交键的有界等待
    if (latest && _hCandEvt) SetEvent(_hCandEvt);
}

// 提交键（空格/Tab/数字/中文标点）专用：最新版本的候选尚未回包时，
// 有界等待（不长期阻塞宿主）。UI → CANDS 通常 1~30ms；超上限即放弃等待，
// 按现路径提交（回退原行为）。管道断开绝不等待——纯直通不能拖慢打键。
void CTextService::WaitForLatestCandidates(DWORD maxMs)
{
    if (!_csInit || !_hCandEvt) return;
    if (!_pipe.IsConnected()) return;
    EnterCriticalSection(&_cs);
    bool pending = (_verCands != _verSent);
    if (pending) ResetEvent(_hCandEvt); // 丢弃历史信号，防陈旧唤醒
    LeaveCriticalSection(&_cs);
    if (pending)
        WaitForSingleObject(_hCandEvt, maxMs);
}

void CTextService::OnPickRequested(uint32_t ver, uint32_t index)
{
    if (_hwndMarshal)
        PostMessageW(_hwndMarshal, WM_APP_LX_PICK, index, ver);
}

void CTextService::HandlePickFromUI(uint32_t ver, uint32_t index)
{
    // 拼音组合中：鼠标选词 = 提交该汉字进组合（cover 支持部分提交，B1）
    if (_pComposition)
    {
        std::wstring hanzi;
        uint32_t cover = 0;
        if (GetCandidateEx(index, hanzi, cover)) PinyinCommitHanzi(hanzi, cover);
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
    _candCover.clear();
    _verCands = 0;
    _replaceLen = 0;
    _pageStart = 0;
    LeaveCriticalSection(&_cs);
}

// _assocShown/_assocReq 跨线程（OnCandidates 管道线程写、宿主 UI 线程读写），一律持 _cs。
bool CTextService::IsAssocShown()
{
    if (!_csInit) return false;
    EnterCriticalSection(&_cs);
    bool shown = _assocShown;
    LeaveCriticalSection(&_cs);
    return shown;
}

void CTextService::ClearAssocState()
{
    if (!_csInit) return;
    EnterCriticalSection(&_cs);
    _assocShown = false;
    _assocReq = false;    // 同步作废在途联想请求：迟到 CANDS 认账即假（防复活）
    LeaveCriticalSection(&_cs);
}

void CTextService::SendHide()
{
    _pipe.Send(MSG_HIDE, NULL, 0);
}

// 翻页：delta=+1 下一页 / -1 上一页。页码为本侧（权威）维护，候选窗只负责
// 按 pageStart 渲染；数字键选词时再叠加 _pageStart 偏移。
void CTextService::PageCandidates(int delta)
{
    if (!_csInit) return;
    uint32_t ver = 0;
    uint32_t newStart = 0;
    bool can = false;
    EnterCriticalSection(&_cs);
    if (!_cands.empty() && _verCands == _verSent)
    {
        uint32_t nxt = NextPageStart(_cands.size(), _pageStart, delta);
        if (nxt != _pageStart)
        {
            _pageStart = nxt;
            newStart = nxt;
            ver = _verSent;
            can = true;
        }
    }
    LeaveCriticalSection(&_cs);
    if (can)
    {
        MsgPage m{ ver, newStart };
        _pipe.Send(MSG_PAGE, &m, sizeof(m));
        LX_LOG(L"ime", L"翻页 ver=%u pageStart=%u", ver, newStart);
    }
}

// Home/End 翻页：last=false 首页 / true 末页。收发模式与 PageCandidates 一致。
void CTextService::PageCandidatesHomeEnd(bool last)
{
    if (!_csInit) return;
    uint32_t ver = 0, newStart = 0;
    bool can = false;
    EnterCriticalSection(&_cs);
    if (!_cands.empty() && _verCands == _verSent)
    {
        uint32_t total = (uint32_t)_cands.size();
        uint32_t target = last ? (total - 1) / kPageSize * kPageSize : 0;
        if (target != _pageStart)
        {
            _pageStart = target;
            newStart = target;
            ver = _verSent;
            can = true;
        }
    }
    LeaveCriticalSection(&_cs);
    if (can)
    {
        MsgPage m{ ver, newStart };
        _pipe.Send(MSG_PAGE, &m, sizeof(m));
        LX_LOG(L"ime", L"Home/End 翻页 ver=%u pageStart=%u", ver, newStart);
    }
}

void CTextService::SendPage()
{
    // 保留：显式重发当前页（当前翻页在 PageCandidates 内即时发送，此函数备用）
    if (!_csInit) return;
    uint32_t ver = 0, start = 0;
    bool ok = false;
    EnterCriticalSection(&_cs);
    if (_verCands == _verSent) { ver = _verSent; start = _pageStart; ok = true; }
    LeaveCriticalSection(&_cs);
    if (ok)
    {
        MsgPage m{ ver, start };
        _pipe.Send(MSG_PAGE, &m, sizeof(m));
    }
}

// 中/英文模式切换提示：发一个字节给 UI，候选窗位置短暂显示"中/英"。
void CTextService::SendModeToast(bool english)
{
    uint8_t e = english ? 1 : 0;
    _pipe.Send(MSG_MODE_TOAST, &e, sizeof(e));
}

// 键盘选字反馈：确认候选版本一致后，把 (ver, 全局索引) 发给 UI，UI 据此自学习。
void CTextService::NotifyPicked(uint32_t globalIndex)
{
    if (!_csInit) return;
    uint32_t ver = 0;
    bool ok = false;
    EnterCriticalSection(&_cs);
    if (_verCands == _verSent && globalIndex < _cands.size())
    {
        ver = _verCands;
        ok = true;
    }
    LeaveCriticalSection(&_cs);
    if (ok)
    {
        MsgPick m{ ver, globalIndex };
        _pipe.Send(MSG_PICKED, &m, sizeof(m));
    }
}

void CTextService::InsertCandidate(uint32_t index)
{
    std::wstring text;
    uint32_t cover = 0;
    {
        EnterCriticalSection(&_cs);
        if (_verCands != _verSent || index >= _cands.size())
        {
            LeaveCriticalSection(&_cs);
            return;
        }
        text = _cands[index];
        cover = index < _candCover.size() ? _candCover[index] : 0;
        LeaveCriticalSection(&_cs);
    }

    // 拼音输入中（点选候选 / 数字键 / 空格经 PICK 到达）：统一走拼音提交，
    // 由其正确清空内存拼音串并结束拼音状态，避免上屏后 PinyinActive 残留。
    if (PinyinActive())
    {
        LX_LOG(L"ime", L"上屏候选[%u] chars=%u cover=%u", index, (uint32_t)text.size(), cover);
        PinyinCommitHanzi(text, cover);
        return;
    }

    LX_LOG(L"ime", L"上屏候选[%u] chars=%u", index, (uint32_t)text.size());
    // ASYNCDONTCARE 在宿主 UI 线程的按键路径上实际为同步执行；
    // _fSelfInsert 覆盖整个调用窗口以过滤自激发的 OnEndEdit。
    _fSelfInsert = TRUE;
    // 从联想窗点选（_assocShown 为真）：上屏后连发下一轮联想（GAP §11）。
    // _assocShown 由管道线程 OnCandidates 在 _cs 内写，读取走锁式访问器。
    const bool assocHit = IsAssocShown();
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
    ClearAssocState();
    if (ok) SendHide();
    if (ok && assocHit) SendAssocCtx();
}

bool CTextService::InsertTextAtSelection(const std::wstring& text)
{
    if (!_pThreadMgr) return false;
    ITfDocumentMgr* pDoc = nullptr;
    if (FAILED(_pThreadMgr->GetFocus(&pDoc)) || !pDoc)
    {
        LX_LOG(L"ime", L"InsertText: 无焦点文档（GetFocus NULL）→ Win32 焦点窗口兜底");
        return InsertViaWin32Focus(text);
    }
    ITfContext* pCtx = nullptr;
    HRESULT hr = pDoc->GetTop(&pCtx);
    pDoc->Release();
    if (FAILED(hr) || !pCtx)
    {
        LX_LOG(L"ime", L"InsertText: GetTop 失败 hr=0x%08X", hr);
        return false;
    }

    _lastSelfInsertTick = GetTickCount64(); // 兜底抑制 Chromium 异步自插入的滞后 OnEndEdit
    CInsertSession* pSession = new CInsertSession(this, pCtx, text);
    HRESULT hrSession = RequestSession(pCtx, _tfClientId, pSession, TF_ES_READWRITE);
    pSession->Release();
    pCtx->Release();
    if (FAILED(hrSession))
        LX_LOG(L"ime", L"InsertText: 会话失败 hr=0x%08X", hrSession);
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

// 切换键是否在 Test 阶段认领（吞下）：真机应用的 TSF 分发只把 Test 时声明
// eaten 的键交给 OnKeyDown/OnKeyUp（deep_test 直调 KeyDown 绕过此门控，故
// 此前测试全绿而真机切换死码）。切换键必须主动认领才有簿记与切换机会；
// 认领了 KeyDown 后 OnTestKeyUp 同步认领 KeyUp，宿主按消息跟踪 Shift
// 状态才不会永久滞留下压态（GetKeyState 类检测不受影响）。
// 放行未认领的键（大写字母、Ctrl+Shift 和弦等）永远到不了 OnKeyDown，
// 因此和弦检测（_shiftUsed/_capsUsed 标记）也必须在 OnTestKeyDown 里做。
static bool ToggleKeyClaim(WPARAM wParam, uint32_t toggleMode, bool pinyinMode)
{
    if (!pinyinMode) return false;
    switch (wParam)
    {
    case VK_SHIFT:   return toggleMode == 1 || toggleMode == 2;
    case VK_CONTROL: return toggleMode == 2 || toggleMode == 4;
    case VK_CAPITAL: return toggleMode == 3;
    }
    return false;
}

STDMETHODIMP CTextService::OnSetFocus(BOOL fForeground)
{
    _fForeground = fForeground;
    if (!fForeground)
    {
        // 失焦：取消进行中的拼音、隐藏候选窗，避免拼音串/候选跨窗口残留。
        if (PinyinActive()) PinyinCancel();
        ClearCandidates();
        SendHide();
    }
    // 焦点切换重置切换键簿记：按住 Shift/Ctrl 期间焦点漂移会丢 KeyUp，
    // 残留 _shiftDown 会把后续所有键标记成和弦，切换长期失灵（自愈复位）
    _shiftDown = false;
    _shiftUsed = false;
    _capsDown = false;
    _capsUsed = false;
    // TIP 前台切换（含同语言内切换到其他输入法）：重判拼音/直通模式
    RequestModeDetect();
    LX_LOG(L"ime", L"KeySink OnSetFocus foreground=%d", fForeground);
    return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
{
    // LINGXI_LOG=1 时的分发取证：是否被调到 + 关键内部态（PITFALLS #19 类问题定位）
    LX_LOG(L"key", L"OnTestKeyDown vk=0x%02X py=%d tmpEn=%d fAct=%d",
           (uint32_t)wParam, (int)_pinyinMode, (int)_tempEnglish, (int)_fActivated);
    // 语言变化（若发生）→ 投递延迟检测；检测本身绝不在按键分发内执行（PITFALLS #18）
    MaybeRedetectPinyinMode();

    // 和弦标记：切换键按住期间任何其他键（含放行键）都算组合使用。
    // 真机门控下放行键（大写字母、Ctrl+Shift+K 的 K 等）不会进 OnKeyDown，
    // 原先只在 OnKeyDown 里标记 _shiftUsed 挡不住这些和弦
    if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_CAPITAL)
    {
        if (_shiftDown) _shiftUsed = true;
        if (_capsDown) _capsUsed = true;
    }

    // 选词联想撤销（GAP §11）：联想候选显示中，任何非选词键立即撤销并放行。
    // 选词键保留（空格/Tab/回车/数字/减号句号=提交、方向键=翻页移动）——对标
    // MS"可空格连续输出"；字母键走新组合自然替换（PinyinStart 清候选），此处
    // 不拦字母（PinyinKeyEaten 在组合中被调用时字母本就被吃）。
    if (IsAssocShown() && !PinyinActive())
    {
        // 只有"选词键"保留联想（空格/Tab=本页首选，数字=第 n 词——与 Down 侧
        // 无组合选词道同一集合，撤销门不得先清窗否则选词道永远打不着）；
        // 回车/方向/翻页键的提交道只在组合中成立，空组合下放行并撤销，
        // 防候选留在屏上语义漂变。
        const bool keepAssoc =
            wParam == VK_SPACE || wParam == VK_TAB ||   // Tab=提交首选（与下方选词道对齐）
            (wParam >= L'1' && wParam <= L'9');
        if (!keepAssoc)
        {
            LX_LOG(L"ime", L"联想撤销 vk=0x%02X", (uint32_t)wParam);
            ClearAssocState();
            ClearCandidates();
            SendHide();
        }
    }
    // 拼音模式优先（字母进组合、组合态拦截编辑键）
    if (PinyinKeyEaten(wParam))
    {
        *pfEaten = TRUE;
        return S_OK;
    }
    // 中英切换键认领（Shift/Ctrl/CapsLock 按 toggleMode）：不认领则真机
    // OnKeyDown/OnKeyUp 全程不被调用，切换死码（原 CapsLock mode3 特判并入）
    if (ToggleKeyClaim(wParam, LoadSettings().toggleMode, _pinyinMode))
    {
        *pfEaten = TRUE;
        return S_OK;
    }
    // 直通形态：候选可见时拦截确认键，其余一律放行
    // （Ctrl/Alt 组合不拦截：候选可见时 Ctrl+1/Ctrl+Tab 等快捷键仍属应用）
    bool visible = CandidatesVisible();
    if (visible) // 逐键日志只记录候选交互期，避免真实使用时刷屏
        LX_LOG(L"key", L"OnTestKeyDown vk=%u visible=1", (uint32_t)wParam);
    // 联想窗（_assocShown）空格也认领（GAP §11 可空格连续输出）：IsSelectionKey
    // 为防误伤英文补全 ghost（空格应入正文）本不含空格，联想态单独加闸；
    // 真正提交在 HandlePinyinKey 空组合联想选词道（复用 InsertCandidate）。
    *pfEaten = (visible && !CtrlOrAltDown() &&
              (IsSelectionKey(wParam) || (IsAssocShown() && wParam == VK_SPACE))) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten)
{
    LX_LOG(L"key", L"OnKeyDown vk=0x%02X py=%d tmpEn=%d fAct=%d",
           (uint32_t)wParam, (int)_pinyinMode, (int)_tempEnglish, (int)_fActivated);
    MaybeRedetectPinyinMode(); // 个别应用不走 TestKeyDown

    // 中英切换键单击检测（toggleMode：1 Shift / 2 Shift 或 Ctrl / 3 CapsLock / 4 Ctrl）：
    // 记录按下；与其他键组合（含切换键交错，如 Ctrl+Shift+K 应用快捷键）则标记 used
    if (wParam == VK_SHIFT || wParam == VK_CONTROL)
    {
        if (_shiftDown)
            _shiftUsed = true; // 两个切换键先后按下 = 组合使用，KeyUp 不切换
        else
        {
            _shiftDown = true;
            _shiftUsed = false;
            _shiftWasActive = PinyinActive();
        }
    }
    else if (wParam == VK_CAPITAL)
    {
        if (_shiftDown)
            _shiftUsed = true; // Shift 未放又按 CapsLock = 组合使用
        if (_capsDown)
            _capsUsed = true; // 自动重复
        else
        {
            _capsDown = true;
            _capsUsed = false;
            _capsWasActive = PinyinActive();
        }
    }
    else
    {
        if (_shiftDown)
            _shiftUsed = true;
        if (_capsDown)
            _capsUsed = true;
    }

    // 认领的切换键统一在此吞下返回：KeyDown 阶段保持 eaten=TRUE（与 Test
    // 一致），不落拼音处理——HandlePinyinKey 对 VK_SHIFT 显式放行
    // （*pfEaten=FALSE），Test 吞/KeyDown 放的不一致会让 TypeKey 断言失败，
    // 宿主也只见 Shift 半程消息（下压滞留）。原 CapsLock mode3 特判并入。
    if (ToggleKeyClaim(wParam, LoadSettings().toggleMode, _pinyinMode))
    {
        *pfEaten = TRUE;
        return S_OK;
    }

    // 中文输入（拼音模式且非临时英文）才走拼音处理
    if (IsChineseInput())
    {
        HandlePinyinKeySEH(this, pContext, wParam, pfEaten); // SEH 包裹（框架 AV 遏制）
        return S_OK;
    }
    // 临时英文模式：字母/标点一律放行；但候选可见时 Test 三面门已认领
    // Tab/数字/Esc（IsSelectionKey 与模式无关），Down 必须给对称出口——
    // 用户 2026-11-02 报"英文输入无法数字选候选词"的根因=旧版在此一律
    // 放行导致被吃的键凭空蒸发。选词道与中文模式同语义：本页第 n 候选。
    if (_tempEnglish)
    {
        if (CandidatesVisible() && !CtrlOrAltDown())
        {
            if (wParam == VK_ESCAPE)
            {
                SendHide();
                ClearCandidates();
                *pfEaten = TRUE;
                return S_OK;
            }
            if (wParam == VK_TAB || (wParam >= L'1' && wParam <= L'9'))
            {
                uint32_t idx;
                bool hit;
                EnterCriticalSection(&_cs);
                idx = (wParam == VK_TAB)
                          ? _pageStart
                          : _pageStart + (uint32_t)(wParam - L'1');
                hit = (_verCands == _verSent && idx < (uint32_t)_cands.size());
                LeaveCriticalSection(&_cs);
                if (hit)
                {
                    InsertCandidate(idx);
                    *pfEaten = TRUE;
                    return S_OK;
                }
            }
        }
        *pfEaten = FALSE;
        return S_OK;
    }
    *pfEaten = FALSE;
    if (!CandidatesVisible()) return S_OK;
    // Ctrl/Alt 组合键直通（同 OnTestKeyDown 的闸）：候选可见时的
    // Ctrl+数字/TAB/Esc 仍是应用快捷键
    if (CtrlOrAltDown()) return S_OK;

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

STDMETHODIMP CTextService::OnTestKeyUp(ITfContext*, WPARAM wParam, LPARAM, BOOL* pfEaten)
{
    // 与 KeyDown 对称认领：已记录在按（_shiftDown/_capsDown，即 KeyDown 已被
    // 认领吞下）的切换键，KeyUp 也认领，OnKeyUp 才有机会执行切换检测；
    // 宿主按消息跟踪 Shift 的也不会只收到一半（滞留下压态）
    bool claim = false;
    if (wParam == VK_SHIFT || wParam == VK_CONTROL)
        claim = _shiftDown && ToggleKeyClaim(wParam, LoadSettings().toggleMode, _pinyinMode);
    else if (wParam == VK_CAPITAL)
        claim = _capsDown && ToggleKeyClaim(wParam, LoadSettings().toggleMode, _pinyinMode);
    *pfEaten = claim ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyUp(ITfContext*, WPARAM wParam, LPARAM, BOOL* pfEaten)
{
    *pfEaten = FALSE;
    // 中英切换键单击（未与其他键组合、按下前后均无拼音活动、当前模式允许该键）：
    // 在中文/临时英文间切换。toggleMode：0 无 1 Shift 2 Shift 或 Ctrl 4 Ctrl。
    if (_shiftDown && (wParam == VK_SHIFT || wParam == VK_CONTROL))
    {
        uint32_t tm = LoadSettings().toggleMode;
        bool allowed = (wParam == VK_SHIFT) ? (tm == 1 || tm == 2)
                                            : (tm == 2 || tm == 4);
        if (allowed && !_shiftUsed && !_shiftWasActive && !PinyinActive() && _pinyinMode)
        {
            _tempEnglish = !_tempEnglish;
            LX_LOG(L"key", L"Shift toggle -> tempEnglish=%d", (int)_tempEnglish);
            SendModeToast(_tempEnglish);
        }
        _shiftDown = false;
        _shiftUsed = false;
    }
    // CapsLock 单击（mode 3）：干净单击切中英；KeyDown 已吞（大小写未翻转）
    if (wParam == VK_CAPITAL && _capsDown)
    {
        uint32_t tm = LoadSettings().toggleMode;
        if (tm == 3 && !_capsUsed && !_capsWasActive && !PinyinActive() && _pinyinMode)
        {
            _tempEnglish = !_tempEnglish;
            LX_LOG(L"key", L"CapsLock toggle -> tempEnglish=%d", (int)_tempEnglish);
            SendModeToast(_tempEnglish);
        }
        _capsDown = false;
        _capsUsed = false;
    }
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
        // 消息泵空闲点：此处调 GetActiveLanguageProfile 远离按键分发与激活回调
        if (svc && svc->_modeDirty.exchange(false))
            DetectPinyinModeSEH(svc);
        return 0;
    }
    if (msg == WM_TIMER)
    {
        auto* svc = (CTextService*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (svc && wParam == (UINT_PTR)kModeRetryTimer)
        {
            // 激活过渡窗口的有限次延迟重检（见 DetectPinyinMode 自愈逻辑）
            svc->RequestModeDetect();
        }
        else if (svc && wParam == (UINT_PTR)kInsertRetryTimer)
        {
            if (svc->_pendingInsert.empty())
            {
                KillTimer(hwnd, kInsertRetryTimer);
                return 0;
            }
            if (svc->PinyinActive()) svc->AbortCompositionState();
            svc->_fSelfInsert = TRUE;
            bool okIns = svc->InsertTextAtSelection(svc->_pendingInsert);
            svc->_fSelfInsert = FALSE;
            if (okIns || ++svc->_insertRetryCount >= kInsertRetryMax)
            {
                LX_LOG(L"ime", L"INSERT 重试%s count=%d",
                       okIns ? L"成功" : L"放弃（焦点未复原）", svc->_insertRetryCount);
                svc->_pendingInsert.clear();
                KillTimer(hwnd, kInsertRetryTimer);
            }
        }
        return 0;
    }
    if (msg == WM_APP_LX_INSERT)
    {
        auto* svc = (CTextService*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        auto* s = (std::wstring*)lParam;
        if (svc && s)
        {
            LX_LOG(L"ime", L"INSERT 收到 chars=%u pinyin=%d", (uint32_t)s->size(), svc->PinyinActive() ? 1 : 0);
            if (svc->PinyinActive()) svc->AbortCompositionState();
            // 守卫整个自插入窗口，避免上屏后 OnEndEdit 误弹上下文预测
            svc->_fSelfInsert = TRUE;
            bool okIns = svc->InsertTextAtSelection(*s);
            svc->_fSelfInsert = FALSE;
            if (!okIns)
            {
                // 面板搜索态提交：插入消息可能先于目标应用的 TSF 焦点复原到达
                // （GetFocus NULL）。暂存并定时重试；面板侧已改为先还前台再发。
                svc->_pendingInsert = *s;
                svc->_insertRetryCount = 0;
                SetTimer(hwnd, kInsertRetryTimer, 30, NULL);
                LX_LOG(L"ime", L"INSERT 失败，转定时重试 chars=%u", (uint32_t)s->size());
            }
        }
        else
        {
            LX_LOG(L"ime", L"INSERT 丢弃 svc=%p s=%p", (void*)svc, (void*)s);
        }
        delete s;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace lingxi
