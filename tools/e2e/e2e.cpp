//
// 灵犀输入法端到端自动化测试宿主。
//
// 原理：实现合成文本存储（ITextStoreACP），程序化创建 TSF 上下文并激活
// 灵犀 TIP，用 ITfKeystrokeMgr 模拟按键（未吃键则按应用语义插入字符），
// 直接读取缓冲区断言结果。覆盖：注册激活、按键直通、上下文读取、IPC、
// 预测、替换/插入上屏、英文与中文两条路径、候选窗真实显示。
//
// 本文件是 GUID 的实例化点（<initguid.h> 必须最先包含）。
//
#include <initguid.h>
#include <windows.h>
#include <msctf.h>
#include <textstor.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../common/guids.h"

static int g_pass = 0;
static int g_fail = 0;

static void Report(bool ok, const char* name, const wchar_t* detail = L"")
{
    wprintf(L"[%s] %hs %s\n", ok ? L"PASS" : L"FAIL", name, detail);
    if (ok) g_pass++; else g_fail++;
}

// ==================== 合成文本存储 ====================

class TestStore : public ITextStoreACP
{
public:
    TestStore() : _cRef(1)
    {
        _sel.acpStart = 0;
        _sel.acpEnd = 0;
        _sel.style.ase = (TsActiveSelEnd)TF_AE_END;
        _sel.style.fInterimChar = FALSE;
    }
    virtual ~TestStore() { if (_sink) _sink->Release(); }

    const std::wstring& Text() const { return _buf; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITextStoreACP))
            *ppv = (ITextStoreACP*)this;
        if (*ppv) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++_cRef; }
    STDMETHODIMP_(ULONG) Release() override { ULONG c = --_cRef; if (!c) delete this; return c; }

    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD) override
    {
        if (!IsEqualIID(riid, IID_ITextStoreACPSink)) return E_INVALIDARG;
        if (_sink) _sink->Release();
        return punk->QueryInterface(IID_ITextStoreACPSink, (void**)&_sink);
    }
    STDMETHODIMP UnadviseSink(IUnknown* punk) override
    {
        if (_sink && punk && (IUnknown*)_sink == punk) { _sink->Release(); _sink = nullptr; }
        return S_OK;
    }
    STDMETHODIMP RequestLock(DWORD dwLockFlags, HRESULT* phrSession) override
    {
        // 测试宿主：无条件立即授予文档锁
        *phrSession = _sink ? _sink->OnLockGranted(dwLockFlags) : S_OK;
        return S_OK;
    }
    STDMETHODIMP GetStatus(TS_STATUS* pdcs) override
    {
        ZeroMemory(pdcs, sizeof(*pdcs));
        return S_OK;
    }
    STDMETHODIMP QueryInsert(LONG acpTestStart, LONG acpTestEnd, ULONG, LONG* pacpResultStart, LONG* pacpResultEnd) override
    {
        *pacpResultStart = acpTestStart;
        *pacpResultEnd = acpTestEnd;
        return S_OK;
    }
    STDMETHODIMP GetSelection(ULONG ulIndex, ULONG ulCount, TS_SELECTION_ACP* pSelection, ULONG* pcFetched) override
    {
        if (ulIndex != TF_DEFAULT_SELECTION || ulCount == 0) return E_INVALIDARG;
        pSelection[0] = _sel;
        *pcFetched = 1;
        return S_OK;
    }
    STDMETHODIMP SetSelection(ULONG ulCount, const TS_SELECTION_ACP* pSelection) override
    {
        if (ulCount != 1) return E_INVALIDARG;
        _sel = pSelection[0];
        return S_OK;
    }
    STDMETHODIMP GetText(LONG acpStart, LONG acpEnd, WCHAR* pchPlain, ULONG cchPlainReq,
                         ULONG* pcchPlainRet, TS_RUNINFO* prgRunInfo, ULONG cRunInfoReq,
                         ULONG* pcRunInfoRet, LONG* pacpNext) override
    {
        if (pcchPlainRet) *pcchPlainRet = 0;
        if (pcRunInfoRet) *pcRunInfoRet = 0;
        if (pacpNext) *pacpNext = acpStart;
        if (acpStart < 0 || (size_t)acpStart > _buf.size()) return TS_E_INVALIDPOS;
        LONG end = (acpEnd == -1) ? (LONG)_buf.size() : acpEnd;
        if (end > (LONG)_buf.size()) end = (LONG)_buf.size();
        LONG len = end - acpStart;
        if (len < 0) return TS_E_INVALIDPOS;

        ULONG copied = 0;
        if (pchPlain && cchPlainReq > 0)
        {
            copied = (ULONG)min((ULONG)len, cchPlainReq);
            wmemcpy(pchPlain, _buf.data() + acpStart, copied);
            if (pcchPlainRet) *pcchPlainRet = copied;
        }
        if (prgRunInfo && cRunInfoReq > 0)
        {
            prgRunInfo[0].type = TS_RT_PLAIN;
            prgRunInfo[0].uCount = (ULONG)len;
            if (pcRunInfoRet) *pcRunInfoRet = 1;
        }
        if (pacpNext) *pacpNext = acpStart + (LONG)copied;
        return S_OK;
    }
    STDMETHODIMP SetText(DWORD, LONG acpStart, LONG acpEnd, const WCHAR* pchText, ULONG cch, TS_TEXTCHANGE* pChange) override
    {
        if (acpEnd == -1) acpEnd = (LONG)_buf.size();
        _buf = _buf.substr(0, acpStart) + std::wstring(pchText, cch) + _buf.substr(acpEnd);
        if (pChange) { pChange->acpStart = acpStart; pChange->acpOldEnd = acpEnd; pChange->acpNewEnd = acpStart + (LONG)cch; }
        _sel.acpStart = _sel.acpEnd = acpStart + (LONG)cch;
        NotifyTextChange();
        return S_OK;
    }
    STDMETHODIMP GetFormattedText(LONG, LONG, IDataObject**) override { return E_NOTIMPL; }
    STDMETHODIMP GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* pfInsertable) override
    {
        *pfInsertable = FALSE;
        return S_OK;
    }
    STDMETHODIMP InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    STDMETHODIMP InsertTextAtSelection(DWORD, const WCHAR* pchText, ULONG cch, LONG* pacpStart, LONG* pacpEnd, TS_TEXTCHANGE* pChange) override
    {
        LONG pos = _sel.acpEnd;
        _buf.insert((size_t)pos, pchText, cch);
        *pacpStart = pos;
        *pacpEnd = pos + (LONG)cch;
        if (pChange) { pChange->acpStart = pos; pChange->acpOldEnd = pos; pChange->acpNewEnd = pos + (LONG)cch; }
        _sel.acpStart = pos;
        _sel.acpEnd = pos + (LONG)cch;
        NotifyTextChange();
        return S_OK;
    }
    STDMETHODIMP InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    STDMETHODIMP RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override { return E_NOTIMPL; }
    STDMETHODIMP RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD, LONG*, BOOL*, LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP GetEndACP(LONG* pacp) override { *pacp = (LONG)_buf.size(); return S_OK; }
    STDMETHODIMP GetActiveView(TsViewCookie* pvcView) override { *pvcView = 1; return S_OK; }
    STDMETHODIMP GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP GetTextExt(TsViewCookie, LONG acpStart, LONG acpEnd, RECT* prc, BOOL* pfClipped) override
    {
        // 假想屏幕坐标：每字符 8px 宽、行高 24px，位于 (40,100)
        prc->left = 40 + 8 * acpStart;
        prc->top = 100;
        prc->right = 40 + 8 * acpEnd;
        prc->bottom = 124;
        *pfClipped = FALSE;
        return S_OK;
    }
    STDMETHODIMP GetScreenExt(TsViewCookie, RECT* prc) override
    {
        prc->left = 0; prc->top = 0; prc->right = 800; prc->bottom = 600;
        return S_OK;
    }
    STDMETHODIMP GetWnd(TsViewCookie, HWND* phwnd) override { *phwnd = NULL; return S_OK; }

private:
    void NotifyTextChange()
    {
        if (_sink) _sink->OnTextChange(0, 0);
    }

    LONG _cRef;
    std::wstring _buf;
    TS_SELECTION_ACP _sel;
    ITextStoreACPSink* _sink = nullptr;
};

// ==================== 编辑会话（模拟应用写文本） ====================

class HEditSession : public ITfEditSession
{
public:
    HEditSession(ITfContext* pCtx, const std::wstring& text)
        : _cRef(1), _pCtx(pCtx), _text(text)
    {
        _pCtx->AddRef();
    }
    ~HEditSession() { _pCtx->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
            *ppv = (ITfEditSession*)this;
        if (*ppv) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++_cRef; }
    STDMETHODIMP_(ULONG) Release() override { ULONG c = --_cRef; if (!c) delete this; return c; }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        ITfInsertAtSelection* pInsert = nullptr;
        if (FAILED(_pCtx->QueryInterface(IID_ITfInsertAtSelection, (void**)&pInsert)))
            return E_FAIL;
        ITfRange* pRange = nullptr;
        HRESULT hr = pInsert->InsertTextAtSelection(ec, 0, _text.c_str(), (LONG)_text.size(), &pRange);
        pInsert->Release();
        if (FAILED(hr)) return hr;
        if (pRange)
        {
            TF_SELECTION sel{};
            sel.range = pRange;
            _pCtx->SetSelection(ec, 1, &sel);
            pRange->Release();
        }
        return S_OK;
    }

private:
    LONG _cRef;
    ITfContext* _pCtx;
    std::wstring _text;
};

// ==================== 测试流程 ====================

static void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static TfClientId g_cid = 0;

// 从焦点文档顶层上下文读全部文本。CTF 可能在文档上推"三明治"镜像上下文，
// IME 的上屏落在镜像里；读焦点上下文才是"用户看到的"真实内容。
class HReadSession : public ITfEditSession
{
public:
    HReadSession(ITfContext* pCtx, std::wstring* pOut)
        : _cRef(1), _pCtx(pCtx), _out(pOut) { _pCtx->AddRef(); }
    ~HReadSession() { _pCtx->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
            *ppv = (ITfEditSession*)this;
        if (*ppv) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++_cRef; }
    STDMETHODIMP_(ULONG) Release() override { ULONG c = --_cRef; if (!c) delete this; return c; }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        ITfRange* pRange = nullptr;
        if (FAILED(_pCtx->GetStart(ec, &pRange)) || !pRange) return E_FAIL;
        LONG moved = 0;
        pRange->ShiftEnd(ec, 8192, &moved, NULL);
        WCHAR buf[8192] = L"";
        ULONG cch = 0;
        if (SUCCEEDED(pRange->GetText(ec, 0, buf, 8192, &cch)))
            _out->assign(buf, cch);
        pRange->Release();
        return S_OK;
    }

private:
    LONG _cRef;
    ITfContext* _pCtx;
    std::wstring* _out;
};

static std::wstring ReadFocusedText(ITfThreadMgr* pTm)
{
    std::wstring out;
    ITfDocumentMgr* pDoc = nullptr;
    if (FAILED(pTm->GetFocus(&pDoc)) || !pDoc) return out;
    ITfContext* pCtx = nullptr;
    if (SUCCEEDED(pDoc->GetTop(&pCtx)) && pCtx)
    {
        auto* pSession = new HReadSession(pCtx, &out);
        HRESULT hrSession = E_FAIL;
        HRESULT hr = pCtx->RequestEditSession(g_cid, pSession, TF_ES_READ | TF_ES_SYNC, &hrSession);
        if (hr == TS_E_SYNCHRONOUS || hrSession == TS_E_SYNCHRONOUS)
            pCtx->RequestEditSession(g_cid, pSession, TF_ES_READ | TF_ES_ASYNCDONTCARE, &hrSession);
        pSession->Release();
        for (int i = 0; i < 10 && out.empty(); i++) { PumpMessages(); Sleep(20); }
        pCtx->Release();
    }
    pDoc->Release();
    return out;
}
// 前置净化编辑会话：把焦点上下文从头全选（≤8192 字符）后清空。
// 用途：冲掉上次尝试残留或环境噪声（用户漏进测试缓冲区的实键）。
class HClearSession : public ITfEditSession
{
public:
    HClearSession(ITfContext* pCtx) : _cRef(1), _pCtx(pCtx) { _pCtx->AddRef(); }
    ~HClearSession() { _pCtx->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
            *ppv = (ITfEditSession*)this;
        if (*ppv) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++_cRef; }
    STDMETHODIMP_(ULONG) Release() override { ULONG c = --_cRef; if (!c) delete this; return c; }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        ITfRange* pRange = nullptr;
        if (FAILED(_pCtx->GetStart(ec, &pRange)) || !pRange) return E_FAIL;
        LONG moved = 0;
        pRange->ShiftEnd(ec, 8192, &moved, NULL);
        HRESULT hr = pRange->SetText(ec, 0, L"", 0);
        pRange->Release();
        return hr;
    }

private:
    LONG _cRef;
    ITfContext* _pCtx;
};

static bool ClearFocusedText(ITfThreadMgr* pTm)
{
    ITfDocumentMgr* pDoc = nullptr;
    if (FAILED(pTm->GetFocus(&pDoc)) || !pDoc) return false;
    ITfContext* pCtx = nullptr;
    bool ok = false;
    if (SUCCEEDED(pDoc->GetTop(&pCtx)) && pCtx)
    {
        auto* pSession = new HClearSession(pCtx);
        HRESULT hrSession = E_FAIL;
        HRESULT hr = pCtx->RequestEditSession(g_cid, pSession, TF_ES_READWRITE | TF_ES_SYNC, &hrSession);
        if (hr == TS_E_SYNCHRONOUS || hrSession == TS_E_SYNCHRONOUS)
            pCtx->RequestEditSession(g_cid, pSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
        pSession->Release();
        ok = SUCCEEDED(hrSession);
        pCtx->Release();
    }
    pDoc->Release();
    return ok;
}

static ITfInputProcessorProfiles* g_pIpp = nullptr;
static ITfThreadMgr* g_pTm = nullptr;
static ITfDocumentMgr* g_pDoc = nullptr;
static HWND g_hwndHost = NULL;

static bool ActivateProfileRetry(LANGID langid, REFGUID profile, int tries = 10)
{
    for (int i = 0; i < tries; i++)
    {
        HRESULT hr = g_pIpp->ActivateLanguageProfile(CLSID_LingxiTextService, langid, profile);
        if (SUCCEEDED(hr)) return true;
        PumpMessages();
        Sleep(300);
    }
    return false;
}

// ctfmon 会与程序化激活竞态：只重试 profile 激活（en-US 优先，zh-CN 兜底——
// 引擎是上下文驱动的，激活哪个 profile 都能测）。绝不反复改窗口焦点，
// 那会诱发 CTF 把文档焦点切到三明治上下文，破坏断言。
static bool ActivateAnyLingxiProfile(int tries = 5)
{
    if (ActivateProfileRetry(0x0409, GUID_LingxiProfile_enUS, tries)) return true;
    if (ActivateProfileRetry(0x0804, GUID_LingxiProfile_zhCN, tries)) return true;
    return false;
}

static void ReassertFocus()
{
    ActivateAnyLingxiProfile();
    PumpMessages();
    Sleep(150);
    PumpMessages();
}

static bool InsertViaTsf(ITfContext* pCtx, const std::wstring& text)
{
    auto* pSession = new HEditSession(pCtx, text);
    HRESULT hrSession = E_FAIL;
    HRESULT hr = pCtx->RequestEditSession(g_cid, pSession, TF_ES_READWRITE | TF_ES_SYNC, &hrSession);
    pSession->Release();
    if (hr == TS_E_SYNCHRONOUS)
    {
        // 同步被拒则用异步 + 消息泵
        pSession = new HEditSession(pCtx, text);
        pCtx->RequestEditSession(g_cid, pSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
        pSession->Release();
        for (int i = 0; i < 20; i++) { PumpMessages(); Sleep(25); }
        return SUCCEEDED(hrSession);
    }
    return SUCCEEDED(hr) && SUCCEEDED(hrSession);
}

static bool WaitTabEaten(ITfKeystrokeMgr* pKm)
{
    for (int i = 0; i < 30; i++)
    {
        PumpMessages();
        BOOL eaten = FALSE;
        HRESULT hr = pKm->TestKeyDown(VK_TAB, 0x000F0001, &eaten);
        if (i == 0 || (i < 5 && FAILED(hr)))
            wprintf(L"[INFO] TestKeyDown(TAB) hr=0x%08X eaten=%d\n", (unsigned)hr, eaten);
        if (SUCCEEDED(hr) && eaten)
        {
            eaten = FALSE;
            hr = pKm->KeyDown(VK_TAB, 0x000F0001, &eaten);
            wprintf(L"[INFO] KeyDown(TAB) hr=0x%08X eaten=%d\n", (unsigned)hr, eaten);
            return eaten != FALSE;
        }
        // ctfmon 会周期性把键盘前台还给系统默认输入法；高频重新激活对抗
        if (i % 2 == 1)
            ActivateAnyLingxiProfile(1);
        Sleep(100);
    }
    return false;
}

// 单次完整测试场景。返回 true = 本次尝试全部断言通过。
// 环境噪声（ctfmon 抢键盘前台、用户实键漏进测试缓冲区）会让个别断言失败：
// 检测到即 fail-fast 立即返回，由外层 wmain 重试整个场景。
static bool RunScenario()
{
    g_pass = 0;
    g_fail = 0;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { Report(false, "CoInitializeEx"); return false; }

    HWND hwndHost = NULL;
    ITfThreadMgr* pTm = nullptr;
    ITfDocumentMgr* pDoc = nullptr;
    ITfContext* pCtx = nullptr;
    TestStore* pStore = nullptr;
    ITfInputProcessorProfiles* pIpp = nullptr;
    ITfKeystrokeMgr* pKm = nullptr;

    do
    {
        // ---- 真实宿主窗口（CTF 的键盘路由依赖真实窗口焦点）----
        HINSTANCE hInst = GetModuleHandleW(NULL);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = L"LingxiE2EHost";
        RegisterClassW(&wc); // 第 2/3 次尝试返回"类已存在"，无碍
        hwndHost = CreateWindowW(L"LingxiE2EHost", L"Lingxi E2E", WS_OVERLAPPEDWINDOW,
                                 60, 60, 420, 320, NULL, NULL, hInst, NULL);
        if (!hwndHost) { Report(false, "Create host window"); break; }
        ShowWindow(hwndHost, SW_SHOW);
        SetForegroundWindow(hwndHost);
        SetFocus(hwndHost);
        PumpMessages();

        // ---- TSF 初始化 ----
        hr = CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void**)&pTm);
        if (FAILED(hr)) { Report(false, "Create TF_ThreadMgr"); break; }

        hr = pTm->Activate(&g_cid);
        Report(SUCCEEDED(hr), "ThreadMgr::Activate");

        pStore = new TestStore();
        hr = pTm->CreateDocumentMgr(&pDoc);
        Report(SUCCEEDED(hr), "CreateDocumentMgr");
        TfEditCookie ecStore = 0;
        hr = pDoc->CreateContext(g_cid, 0, pStore, &pCtx, &ecStore);
        Report(SUCCEEDED(hr), "CreateContext");
        if (pCtx) { pDoc->Push(pCtx); pCtx->Release(); }
        if (!pCtx) break; // 无上下文无法继续（CreateContext 已报 FAIL）

        // 关键：窗口 <-> 文档管理器关联（官方 tsfapp 同款）。
        // CTF 监听该窗口的焦点变化来维护键盘路由状态。
        ITfDocumentMgr* pPrevDoc = nullptr;
        hr = pTm->AssociateFocus(hwndHost, pDoc, &pPrevDoc);
        Report(SUCCEEDED(hr), "AssociateFocus");
        if (pPrevDoc) pPrevDoc->Release();
        SetFocus(hwndHost);
        PumpMessages();

        // ---- 激活灵犀 TIP（en-US）----
        CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                         IID_ITfInputProcessorProfiles, (void**)&pIpp);
        if (!pIpp) { Report(false, "Create InputProcessorProfiles"); break; }
        g_pIpp = pIpp;
        g_pTm = pTm;
        g_pDoc = pDoc;
        g_hwndHost = hwndHost;

        Report(ActivateAnyLingxiProfile(10), "ActivateLanguageProfile (Lingxi TIP)");

        Sleep(1200); // DLL Activate -> 拉起 LingxiUI -> 管道连接

        pTm->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm);
        if (!pKm) { Report(false, "QueryInterface ITfKeystrokeMgr"); break; }

        // 诊断：CTF 认为的键盘前台 TIP
        {
            CLSID fg = {};
            HRESULT hrFg = pKm->GetForeground(&fg);
            wchar_t detail[96];
            if (SUCCEEDED(hrFg) && IsEqualCLSID(fg, CLSID_LingxiTextService))
                _snwprintf_s(detail, _TRUNCATE, L"foreground = Lingxi (OK)");
            else if (SUCCEEDED(hrFg))
                _snwprintf_s(detail, _TRUNCATE, L"foreground = other TIP");
            else
                _snwprintf_s(detail, _TRUNCATE, L"GetForeground hr=0x%08X", (unsigned)hrFg);
            wprintf(L"[INFO] %ls\n", detail);
        }

        // ---- 前置净化：焦点文本非空（残留/用户漏入按键）则全选删除，
        //      保证 pass-through 断言从干净缓冲区开始 ----
        {
            std::wstring pre = ReadFocusedText(pTm);
            if (!pre.empty())
            {
                wprintf(L"[INFO] pre-sanitize: focused has %zu chars, clearing\n", pre.size());
                ClearFocusedText(pTm);
                PumpMessages();
                wprintf(L"[INFO] post-sanitize: focused='%ls'\n", ReadFocusedText(pTm).c_str());
            }
        }

        // ---- 测试 1：打字直通 ----
        ReassertFocus();
        bool passThrough = true;
        for (wchar_t ch : std::wstring(L"mor"))
        {
            BOOL eaten = TRUE;
            if (FAILED(pKm->TestKeyDown((WPARAM)ch, 0x00260001, &eaten)) || eaten) { passThrough = false; break; }
            if (!InsertViaTsf(pCtx, std::wstring(1, ch))) { passThrough = false; break; }
            PumpMessages();
        }
        std::wstring t = ReadFocusedText(pTm);
        bool passThroughOk = passThrough && t == L"mor";
        Report(passThroughOk, "typing pass-through", t.c_str());
        if (!passThroughOk) break; // fail-fast：缓冲区被噪声污染，交给外层重试

        // 诊断：双路读数 + 上下文指针对照
        {
            ITfDocumentMgr* pFocDoc = nullptr;
            ITfContext* pFocCtx = nullptr;
            pTm->GetFocus(&pFocDoc);
            if (pFocDoc) { pFocDoc->GetTop(&pFocCtx); }
            wprintf(L"[INFO] store='%ls' focused='%ls' ourCtx=%p focusCtx=%p\n",
                    pStore->Text().c_str(), ReadFocusedText(pTm).c_str(),
                    (void*)pCtx, (void*)pFocCtx);
            if (pFocCtx) pFocCtx->Release();
            if (pFocDoc) pFocDoc->Release();
        }

        // ---- 测试 2：候选窗真实显示 ----
        bool wndShown = false;
        for (int i = 0; i < 30; i++)
        {
            PumpMessages();
            HWND hwnd = FindWindowW(L"LingxiCandidateWnd", NULL);
            if (hwnd && IsWindowVisible(hwnd))
            {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                wchar_t detail[128];
                _snwprintf_s(detail, _TRUNCATE, L"rect=(%ld,%ld,%ld,%ld)", rc.left, rc.top, rc.right, rc.bottom);
                Report(true, "candidate window visible", detail);
                wndShown = true;
                break;
            }
            Sleep(100);
        }
        if (!wndShown)
        {
            Report(false, "candidate window visible");
            break; // fail-fast
        }

        // ---- 测试 3：Tab 上屏（替换前缀；真词表下 "mor" 的最高频候选是 more）----
        bool tabEaten = WaitTabEaten(pKm);
        Report(tabEaten, "Tab eaten by IME");
        if (!tabEaten) break; // fail-fast
        PumpMessages();
        Sleep(300);
        PumpMessages();
        t = ReadFocusedText(pTm);
        bool moreOk = (t == L"more");
        Report(moreOk, "prefix replaced: mor -> more", t.c_str());
        if (!moreOk) break; // fail-fast

        // ---- 测试 4：直通完整性（Tab 之后继续正常输入）----
        {
            BOOL eaten = TRUE;
            pKm->TestKeyDown((WPARAM)'X', 0x002D0001, &eaten);
            if (!eaten) InsertViaTsf(pCtx, L"X");
            PumpMessages();
            t = ReadFocusedText(pTm);
            bool postOk = (t == L"moreX");
            Report(postOk, "post-commit pass-through", t.c_str());
            if (!postOk) break; // fail-fast
        }

        // ---- 测试 4b：真词表引擎 —— 空格后输入 "inte" 应替换为 interest 等高频词 ----
        {
            BOOL e = TRUE;
            pKm->TestKeyDown(VK_SPACE, 0x00390001, &e);
            if (!e) InsertViaTsf(pCtx, L" ");
            PumpMessages();
            bool okTyping = true;
            for (wchar_t ch : std::wstring(L"inte"))
            {
                e = TRUE;
                pKm->TestKeyDown((WPARAM)ch, 0x00260001, &e);
                if (!e && !InsertViaTsf(pCtx, std::wstring(1, ch))) { okTyping = false; break; }
                PumpMessages();
            }
            // 关键断言前置检查：打字应落在预期上下文（moreX inte），否则本 attempt 作废
            t = ReadFocusedText(pTm);
            if (!okTyping || t.compare(0, 10, L"moreX inte") != 0)
            {
                Report(false, "real dict prefix replace: inte -> ...", t.c_str());
                break; // fail-fast
            }
            tabEaten = WaitTabEaten(pKm);
            PumpMessages();
            Sleep(300);
            PumpMessages();
            t = ReadFocusedText(pTm);
            // 引擎选 g10k 词频最高的 inte* 词（当前是 international），断言"发生了
            // inte 开头的补全替换"而非特定词
            bool hit = t.size() > 10 && t.compare(0, 6, L"moreX ") == 0 &&
                       t.compare(6, 4, L"inte") == 0;
            Report(tabEaten && hit, "real dict prefix replace: inte -> ...", t.c_str());
            if (!(tabEaten && hit)) break; // fail-fast
        }

        // ---- 测试 5：中文上下文续写（zh-CN profile；激活失败不影响引擎测试）----
        if (!ActivateProfileRetry(0x0804, GUID_LingxiProfile_zhCN, 3))
            wprintf(L"[WARN] ActivateLanguageProfile zh-CN failed, continue with current profile\n");
        ReassertFocus();
        Sleep(800); // TIP 重激活 + 管道重连

        if (InsertViaTsf(pCtx, L"今天天气不错"))
        {
            size_t sizeBefore = ReadFocusedText(pTm).size();
            tabEaten = WaitTabEaten(pKm);
            Report(tabEaten, "Tab eaten (Chinese)");
            if (!tabEaten) break; // fail-fast
            PumpMessages();
            Sleep(300);
            PumpMessages();
            t = ReadFocusedText(pTm);
            bool chineseOk = t.size() > sizeBefore; // 高频接续词已插入
            Report(chineseOk, "Chinese continuation committed", t.c_str());
            if (!chineseOk) break; // fail-fast
        }
        else
        {
            Report(false, "Chinese context inserted");
            break; // fail-fast
        }
    } while (false);

    // ---- 清理（按获取逆序；fail-fast 也会走到这里，均允许空指针）----
    if (pKm) pKm->Release();
    if (pIpp) pIpp->Release();
    if (pDoc) pDoc->Release();
    if (pStore) pStore->Release();
    if (pTm) { pTm->Deactivate(); pTm->Release(); }
    if (hwndHost) DestroyWindow(hwndHost); // 多次尝试不留残窗抢焦点
    CoUninitialize();

    wprintf(L"=== 结果: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    return g_fail == 0;
}

int wmain()
{
    // 环境噪声重试壳：这台机器正被用户实时使用，ctfmon 会与程序化激活抢键盘
    // 前台，用户实键也可能漏进测试缓冲区。整个场景最多尝试 3 次；每次尝试前
    // 清掉日志与 LingxiUI.exe，保证干净起点；一次全绿即通过（exit 0）。
    wchar_t logPath[MAX_PATH] = L"";
    {
        wchar_t tmpDir[MAX_PATH] = L"";
        if (GetTempPathW(MAX_PATH, tmpDir))
            _snwprintf_s(logPath, _TRUNCATE, L"%lslingxi-ime.log", tmpDir);
    }

    for (int attempt = 1; attempt <= 3; attempt++)
    {
        wprintf(L"=== e2e attempt %d/3 ===\n", attempt);
        if (logPath[0]) DeleteFileW(logPath);
        system("taskkill /IM LingxiUI.exe /F >nul 2>nul");
        Sleep(800);
        if (RunScenario())
            return 0;
        wprintf(L"[WARN] attempt %d failed (PASS=%d FAIL=%d), retrying\n",
                attempt, g_pass, g_fail);
    }

    wprintf(L"[FAIL] 3 attempts all failed\n");
    wprintf(L"[HINT] 多为环境干扰：测试期间请停止打字/切换输入法（ctfmon 会抢键盘前台），\n");
    wprintf(L"       稍后重跑；日志见 %%TEMP%%\\lingxi-ime.log。\n");
    return 1;
}
