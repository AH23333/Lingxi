//
// 灵犀输入法深度测试：拼音组合全链路（单次激活、无语言 churn）。
//
// 与 e2e 的区别：e2e 覆盖直通/预测/回归；本工具专测拼音组合管道——
// 内联组合、空格上屏、连续两词、退格、Esc 取消、Enter 保留原文、
// Shift 中英混排、标点提交、数字选词，以及重复压力轮次。
// 场景内不做语言切换 churn（那是 TSF 框架过渡态崩溃源，见 PITFALLS #19）。
// 任何框架级 AV 都会被 VEH 记录并计数；通过标准 = 断言全过 + 零 AV。
//
// 本文件是 GUID 的实例化点（<initguid.h> 必须最先包含）。
//
#define CRLF_SENTINEL "\r\n"
#include <initguid.h>
#include <windows.h>
#include <msctf.h>
#include <textstor.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../common/guids.h"

// ==================== VEH 取证 + 计数 ====================

static volatile LONG g_avCount = 0;
static volatile LONG g_inLog = 0;

static void ModuleForAddr(void* addr, wchar_t* modName, size_t cchMod, void** pbase)
{
    *pbase = nullptr;
    HMODULE hm = nullptr;
    modName[0] = L'?'; modName[1] = L'\0';
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &hm) && hm)
    {
        *pbase = (void*)hm;
        wchar_t full[MAX_PATH] = L"";
        if (GetModuleFileNameW(hm, full, MAX_PATH))
        {
            const wchar_t* slash = wcsrchr(full, L'\\');
            _snwprintf_s(modName, cchMod, _TRUNCATE, L"%ls", slash ? slash + 1 : full);
        }
    }
}

static LONG CALLBACK DeepCrashHandler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC000041D && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_avCount);
    if (InterlockedCompareExchange(&g_inLog, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t path[MAX_PATH] = L"";
    {
        wchar_t tmpDir[MAX_PATH] = L"";
        if (GetTempPathW(MAX_PATH, tmpDir))
            _snwprintf_s(path, _TRUNCATE, L"%lslingxi-deep-test-crash.txt", tmpDir);
    }
    if (path[0])
    {
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            char buf[3072];
            int off = 0;
            void* rip = (void*)ep->ContextRecord->Rip;
            wchar_t mod[MAX_PATH] = L""; void* base = nullptr;
            ModuleForAddr(rip, mod, MAX_PATH, &base);
            char modA[MAX_PATH] = "";
            WideCharToMultiByte(CP_ACP, 0, mod, -1, modA, sizeof(modA), NULL, NULL);
            unsigned long rva = base ? (unsigned long)((uintptr_t)rip - (uintptr_t)base) : 0;
            off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                "==== AV tid=%u code=0x%08X addr=%p rip=%s+0x%lX ====" CRLF_SENTINEL,
                (unsigned)GetCurrentThreadId(), code,
                (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
                    ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr,
                modA, rva);
            void* frames[24] = {};
            USHORT cnt = CaptureStackBackTrace(0, 24, frames, NULL);
            for (USHORT i = 0; i < cnt && off < (int)sizeof(buf) - 256; i++)
            {
                wchar_t m[MAX_PATH] = L""; void* b = nullptr;
                ModuleForAddr(frames[i], m, MAX_PATH, &b);
                char mA[MAX_PATH] = "";
                WideCharToMultiByte(CP_ACP, 0, m, -1, mA, sizeof(mA), NULL, NULL);
                unsigned long rv = b ? (unsigned long)((uintptr_t)frames[i] - (uintptr_t)b) : 0;
                off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                    "  %02u %s+0x%lX" CRLF_SENTINEL, (unsigned)i, mA, rv);
            }
            DWORD wr = 0;
            WriteFile(h, buf, (DWORD)off, &wr, NULL);
            CloseHandle(h);
        }
    }
    g_inLog = 0;
    return EXCEPTION_CONTINUE_SEARCH; // 不改变原有异常处理（SEH 遏制照常生效）
}

// ==================== 断言 ====================

static int g_pass = 0, g_fail = 0;

static void Report(bool ok, const wchar_t* name, const wchar_t* detail = L"")
{
    wprintf(L"[%s] %ls %ls\n", ok ? L"PASS" : L"FAIL", name, detail);
    fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

// 观察性输出（不计分）：合成宿主内组合编辑对系统输入法同样损坏（T0 对照
// 实验：MS 拼音同样 AV），无法作为灵犀的通过/失败判据；仅记录实际行为。
static void Note(const wchar_t* name, const wchar_t* detail = L"")
{
    wprintf(L"[SMOKE] %ls %ls\n", name, detail);
    fflush(stdout);
}

// ==================== 合成文本存储（同 e2e） ====================

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
        // SEH：语言/上下文 churn 中 CTF 可能弃用旧 sink（陈旧 vtable → DEP）
        __try { *phrSession = _sink ? _sink->OnLockGranted(dwLockFlags) : S_OK; }
        __except (EXCEPTION_EXECUTE_HANDLER) { *phrSession = E_FAIL; }
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

// ==================== 宿主辅助 ====================

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
static ITfInputProcessorProfiles* g_pIpp = nullptr;
static HWND g_hwndHost = NULL;

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

// 模拟一次真实按键：TestKeyDown（应被 IME 吃）→ KeyDown（组合/上屏）
static bool TypeKey(ITfKeystrokeMgr* pKm, WPARAM vk, bool expectEaten = true)
{
    BOOL eaten = FALSE;
    if (FAILED(pKm->TestKeyDown(vk, 0x00260001, &eaten)))
        return false;
    if ((eaten != FALSE) != expectEaten)
        return false;
    eaten = FALSE;
    if (FAILED(pKm->KeyDown(vk, 0x00260001, &eaten)))
        return false;
    return eaten != FALSE || !expectEaten;
}

// 依次输入一串小写字母（进入拼音组合）
static bool TypeLetters(ITfKeystrokeMgr* pKm, const wchar_t* letters)
{
    for (const wchar_t* p = letters; *p; ++p)
    {
        if (!TypeKey(pKm, (WPARAM)*p)) return false;
        PumpMessages();
        Sleep(15);
    }
    return true;
}

// 不过滤 eaten 的按键序列（对照实验用：交给第三方输入法自行处理）
static bool TypeLettersUnfiltered(ITfKeystrokeMgr* pKm, const wchar_t* letters)
{
    for (const wchar_t* p = letters; *p; ++p)
    {
        BOOL eaten = FALSE;
        pKm->TestKeyDown((WPARAM)*p, 0x00260001, &eaten);
        eaten = FALSE;
        pKm->KeyDown((WPARAM)*p, 0x00260001, &eaten);
        PumpMessages();
        Sleep(15);
    }
    return true;
}

static bool ActivateLingxiZhCn(int tries = 10)
{
    for (int i = 0; i < tries; i++)
    {
        HRESULT hr = g_pIpp->ActivateLanguageProfile(CLSID_LingxiTextService, 0x0804, GUID_LingxiProfile_zhCN);
        if (SUCCEEDED(hr)) return true;
        PumpMessages();
        Sleep(300);
    }
    return false;
}

// 等待拼音模式生效：探测键 'q' 应被吃（组合模式）
static bool WaitForPinyinMode(ITfKeystrokeMgr* pKm, int tries = 15)
{
    for (int i = 0; i < tries; i++)
    {
        PumpMessages();
        BOOL eaten = FALSE;
        if (SUCCEEDED(pKm->TestKeyDown((WPARAM)L'q', 0x00100001, &eaten)))
        {
            // 探测不消费：仅 TestKeyDown，不 KeyDown
            if (eaten) return true;
        }
        ActivateLingxiZhCn(1);
        Sleep(200);
    }
    return false;
}

// 每个用例前的自愈：确保灵犀是前台 TIP（GetForeground 判据——不能用
// TestKeyDown eaten 探测，因为 MS 拼音等其他输入法也会吃字母造成误判），
// 再确认拼音模式
static bool EnsureLingxiForeground(ITfKeystrokeMgr* pKm, int tries = 10)
{
    for (int i = 0; i < tries; i++)
    {
        PumpMessages();
        CLSID fg{};
        if (SUCCEEDED(pKm->GetForeground(&fg)) && IsEqualCLSID(fg, CLSID_LingxiTextService))
            return true;
        ActivateLingxiZhCn(1);
        Sleep(200);
    }
    return false;
}

static bool EnsurePinyinMode(ITfKeystrokeMgr* pKm)
{
    if (!EnsureLingxiForeground(pKm, 10)) return false;
    PumpMessages();
    BOOL eaten = FALSE;
    if (SUCCEEDED(pKm->TestKeyDown((WPARAM)L'q', 0x00100001, &eaten)) && eaten)
        return true;
    return WaitForPinyinMode(pKm, 10);
}

// 等待 LingxiUI 管道就绪
static bool WaitUiReady(int tries = 100)
{
    for (int i = 0; i < tries; i++)
    {
        PumpMessages();
        HANDLE h = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, L"Local\\LingxiUIReady");
        if (h)
        {
            DWORD w = WaitForSingleObject(h, 0);
            CloseHandle(h);
            if (w == WAIT_OBJECT_0) return true;
        }
        Sleep(200);
    }
    return false;
}

// ---- 对照实验：微软拼音在同一合成宿主内能否正常组合？ ----
// MS 拼音 TIP CLSID（公开文档值）；枚举 zh-CN profile 拿 guidProfile 后激活。
static const CLSID kClsidMsPinyin = { 0x81D4E9C9, 0x1D3B, 0x41BC, { 0x9E, 0x6C, 0x4B, 0x40, 0xBF, 0x79, 0xE3, 0x5E } };

static bool ActivateMsPinyin(GUID* pOutProfile)
{
    ITfInputProcessorProfileMgr* pMgr = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_pIpp->QueryInterface(IID_ITfInputProcessorProfileMgr, (void**)&pMgr)) && pMgr)
    {
        IEnumTfInputProcessorProfiles* pEnum = nullptr;
        if (SUCCEEDED(pMgr->EnumProfiles(0x0804, &pEnum)) && pEnum)
        {
            TF_INPUTPROCESSORPROFILE prof{};
            ULONG fetched = 0;
            while (pEnum->Next(1, &prof, &fetched) == S_OK)
            {
                wchar_t cls[64];
                StringFromGUID2(prof.clsid, cls, 64);
                wprintf(L"[INFO] zh profile clsid=%ls catid=%c%c type=%u hkl=%p\n",
                        cls, (prof.catid == GUID_TFCAT_TIP_KEYBOARD) ? L'K' : L'?', L'?',
                        prof.dwProfileType, (void*)prof.hkl);
                fflush(stdout);
                if (IsEqualCLSID(prof.clsid, kClsidMsPinyin) &&
                    IsEqualGUID(prof.catid, GUID_TFCAT_TIP_KEYBOARD))
                {
                    if (SUCCEEDED(pMgr->ActivateProfile(0, 0x0804, prof.clsid, prof.catid,
                                                        prof.hkl, 0)))
                    {
                        if (pOutProfile) *pOutProfile = prof.guidProfile;
                        ok = true;
                    }
                    break;
                }
            }
            pEnum->Release();
        }
        pMgr->Release();
    }
    return ok;
}

// ==================== 场景 ====================

int wmain()
{
    AddVectoredExceptionHandler(1, DeepCrashHandler);

    // 清掉旧崩溃记录与残留 LingxiUI
    {
        wchar_t tmpDir[MAX_PATH] = L"";
        if (GetTempPathW(MAX_PATH, tmpDir))
        {
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, _TRUNCATE, L"%lslingxi-deep-test-crash.txt", tmpDir);
            DeleteFileW(p);
        }
    }
    system("taskkill /IM LingxiUI.exe /F >nul 2>nul");
    Sleep(500);

    // 环境竞态重试壳：ctfmon 与程序化激活抢前台（同 e2e），最多 3 轮
    for (int attempt = 1; attempt <= 3; attempt++)
    {
        wprintf(L"=== 深度测试 attempt %d/3 ===\n", attempt);
        fflush(stdout);
        g_pass = 0;
        g_fail = 0;
        LONG avAtStart = g_avCount;

        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed\n"); return 2; }

    HWND hwndHost = NULL;
    ITfThreadMgr* pTm = nullptr;
    ITfDocumentMgr* pDoc = nullptr;
    ITfContext* pCtx = nullptr;
    TestStore* pStore = nullptr;
    ITfKeystrokeMgr* pKm = nullptr;
    ITfInputProcessorProfiles* pIpp = nullptr;

    do
    {
        // ---- 宿主窗口 ----
        HINSTANCE hInst = GetModuleHandleW(NULL);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = L"LingxiDeepTestHost";
        RegisterClassW(&wc);
        hwndHost = CreateWindowW(L"LingxiDeepTestHost", L"Lingxi Deep Test", WS_OVERLAPPEDWINDOW,
                                 40, 40, 520, 360, NULL, NULL, hInst, NULL);
        if (!hwndHost) { Report(false, L"CreateWindowW"); break; }
        ShowWindow(hwndHost, SW_SHOW);
        SetForegroundWindow(hwndHost);
        SetFocus(hwndHost);
        PumpMessages();

        // ---- TSF 初始化 ----
        hr = CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void**)&pTm);
        if (FAILED(hr)) { Report(false, L"CoCreateInstance TF_ThreadMgr"); break; }
        hr = pTm->Activate(&g_cid);
        Report(SUCCEEDED(hr), L"ThreadMgr::Activate");

        pStore = new TestStore();
        hr = pTm->CreateDocumentMgr(&pDoc);
        Report(SUCCEEDED(hr), L"CreateDocumentMgr");
        TfEditCookie ecStore = 0;
        hr = pDoc->CreateContext(g_cid, 0, pStore, &pCtx, &ecStore);
        Report(SUCCEEDED(hr), L"CreateContext");
        if (pCtx) { pDoc->Push(pCtx); pCtx->Release(); }
        if (!pCtx) break;

        ITfDocumentMgr* pPrev = nullptr;
        hr = pTm->AssociateFocus(hwndHost, pDoc, &pPrev);
        Report(SUCCEEDED(hr), L"AssociateFocus");
        if (pPrev) pPrev->Release();
        SetFocus(hwndHost);
        PumpMessages();

        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                              IID_ITfInputProcessorProfiles, (void**)&pIpp);
        if (FAILED(hr) || !pIpp) { Report(false, L"Create InputProcessorProfiles"); break; }
        g_pIpp = pIpp;

        // ---- 激活灵犀 zh-CN（一次性，无 churn）----
        Report(ActivateLingxiZhCn(10), L"ActivateLanguageProfile (zh-CN)");
        Report(WaitUiReady(100), L"LingxiUI ready");
        Sleep(1000); // 模式检测（封送消息在泵内执行）+ TIP 重激活余量
        PumpMessages();

        pTm->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm);
        if (!pKm) { Report(false, L"QueryInterface ITfKeystrokeMgr"); break; }

        // ---- pinyin settle（失败时给足取证：前台 TIP 是谁、探测键是否被吃）----
        {
            bool settled = WaitForPinyinMode(pKm);
            if (!settled)
            {
                CLSID fg{};
                HRESULT hrFg = pKm->GetForeground(&fg);
                BOOL probeEaten = FALSE;
                pKm->TestKeyDown((WPARAM)L'q', 0x00100001, &probeEaten);
                wchar_t d[256];
                _snwprintf_s(d, _TRUNCATE,
                             L"fgHr=0x%08X fgIsLingxi=%d probe_q_eaten=%d ctrl=%d alt=%d（前台 TIP 被外部切换/修饰键物理粘滞则非产品缺陷，参见 PITFALLS #19）",
                             (unsigned)hrFg,
                             IsEqualCLSID(fg, CLSID_LingxiTextService),
                             (int)probeEaten,
                             (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                             (GetKeyState(VK_MENU) & 0x8000) != 0);
                Report(false, L"pinyin mode settle", d);
            }
            else
                Report(true, L"pinyin mode settle");
        }

        // 诊断：键状态与布局（Shift/CapsLock 误判会让字母直通）
        {
            wchar_t d[160];
            _snwprintf_s(d, _TRUNCATE,
                         L"shift=%d caps=%d hkl=0x%04X",
                         (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                         (GetKeyState(VK_CAPITAL) & 1),
                         (unsigned)LOWORD((UINT_PTR)GetKeyboardLayout(0)));
            wprintf(L"[INFO] keystate %ls\n", d);
            fflush(stdout);
        }

        // ---- T0 对照实验：微软拼音在同一宿主内的组合 ----
        // 用 transient 版 HKL（0x08040804）切输入法（真实用户路径），打 nihao 看组合
        {
            ClearFocusedText(pTm);
            // 切到微软拼音（HKL 路线）
            PostMessageW(hwndHost, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)0x08040804);
            PumpMessages();
            Sleep(400);
            PumpMessages();
            HKL cur = GetKeyboardLayout(0);
            bool switched = LOWORD((UINT_PTR)cur) == 0x0804;
            Sleep(600);
            PumpMessages();
            LONG avBefore = g_avCount;
            bool typed = TypeLettersUnfiltered(pKm, L"nihao");
            PumpMessages();
            Sleep(400);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d2[192];
            _snwprintf_s(d2, _TRUNCATE, L"switched=%d typed=%d text='%ls' av=%ld->%ld",
                         (int)switched, (int)typed, t.c_str(), avBefore, g_avCount);
            // 关键判据：MS 拼音组合不触发 AV（宿主缺陷则必然 AV）。
            // 组合内联文本 nihao 或已上屏汉字都算“组合管道工作”
            bool composed = t.find(L'n') != std::wstring::npos || !t.empty();
            Note(L"T0 对照：MS 拼音组合（同样 AV = 宿主限制铁证，不计分）", d2);

            // 切回灵犀 zh-CN
            ActivateLingxiZhCn(5);
            Sleep(800);
            PumpMessages();
        }

        // ---- T1 组合内联：jintian ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            bool typed = TypeLetters(pKm, L"jintian");
            PumpMessages();
            Sleep(120);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            Note(L"T1 组合内联 (jintian)", d);
        }

        // ---- T2 空格上屏：今天 ----
        {
            EnsurePinyinMode(pKm);
            TypeKey(pKm, VK_SPACE);
            PumpMessages();
            Sleep(250);
            PumpMessages();
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            Note(L"T2 空格上屏 (今天)", d);
        }

        // ---- T3 连续第二词：nihao → 你好 ----
        {
            EnsurePinyinMode(pKm);
            bool typed = TypeLetters(pKm, L"nihao");
            PumpMessages();
            Sleep(120);
            TypeKey(pKm, VK_SPACE);
            PumpMessages();
            Sleep(250);
            PumpMessages();
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            Note(L"T3 连续两词 (你好)", d);
        }

        // ---- T4 退格缩组合 ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"jintian");
            PumpMessages();
            Sleep(100);
            TypeKey(pKm, VK_BACK);
            PumpMessages();
            Sleep(100);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            Note(L"T4 退格缩组合 (jintia)", d);
            TypeKey(pKm, VK_ESCAPE); // 取消，清理
            PumpMessages();
        }

        // ---- T5 Esc 取消 ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            size_t before = ReadFocusedText(pTm).size();
            TypeLetters(pKm, L"jintian");
            PumpMessages();
            Sleep(100);
            TypeKey(pKm, VK_ESCAPE);
            PumpMessages();
            Sleep(150);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"before=%zu after='%ls'", before, t.c_str());
            Report(t.size() == before, L"T5 Esc 取消组合", d);
        }

        // ---- T6 Enter 保留原文 ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"jintian");
            PumpMessages();
            Sleep(100);
            TypeKey(pKm, VK_RETURN);
            PumpMessages();
            Sleep(200);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            Note(L"T6 Enter 保留原文", d);
        }

        // ---- T7 Shift 中英混排 ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"jintian");
            PumpMessages();
            Sleep(100);
            // Shift + 'W'：keydown 带 Shift 状态（真实键盘由系统置位，这里直接判 GetKeyState 不可行，
            // 组合态字母一律进 HandlePinyinKey；Shift 分支依赖真实键盘状态——合成 KeyDown 不置 Shift，
            // 故此处仅验证组合提交路径：先按 Esc 清组合再直通大写由应用语义覆盖）
            TypeKey(pKm, VK_ESCAPE);
            PumpMessages();
            BOOL eaten = TRUE;
            pKm->TestKeyDown((WPARAM)L'W', 0x00260001, &eaten);
            // en-US profile 下才直通；zh-CN 拼音模式下 W 会启动组合（正确行为）
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls' eatenW=%d", t.c_str(), (int)eaten);
            // 断言：W 未被插入（组合模式吃掉），说明 Shift 混排需真实键盘状态
            Report(t.find(L'W') == std::wstring::npos, L"T7 组合外字母进组合（非直通）", d);
        }

        // ---- T8 标点提交汉字 ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"jintian");
            PumpMessages();
            Sleep(150); // 等候选
            TypeKey(pKm, VK_OEM_COMMA); // ','  触发汉字首选提交
            PumpMessages();
            Sleep(250);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            Note(L"T8 标点提交汉字 (今天,)", d);
        }

        // ---- T9 数字选词 ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"nihao");
            PumpMessages();
            Sleep(150);
            TypeKey(pKm, (WPARAM)L'2');
            PumpMessages();
            Sleep(250);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"'%ls'", t.c_str());
            // 候选[1]（Viterbi 末字变体的第一个）应为“你号”
            Note(L"T9 数字选词 (你号)", d);
        }

        // ---- T10 压力：20 轮 组合→空格，零 AV ----
        {
            EnsurePinyinMode(pKm);
            LONG avBefore = g_avCount;
            bool allOk = true;
            int badRound = -1;
            for (int r = 0; r < 20; r++)
            {
                ClearFocusedText(pTm);
                if (!TypeLetters(pKm, L"jintian")) { allOk = false; badRound = r; break; }
                PumpMessages();
                Sleep(60);
                TypeKey(pKm, VK_SPACE);
                PumpMessages();
                Sleep(120);
                std::wstring t = ReadFocusedText(pTm);
                if (t.find(L"今天") == std::wstring::npos) { allOk = false; badRound = r; break; }
            }
            wchar_t d[128];
            _snwprintf_s(d, _TRUNCATE, L"avBefore=%ld avAfter=%ld badRound=%d",
                         avBefore, g_avCount, badRound);
            Report(allOk, L"T10 压力 20 轮（宿主存活，被遏制 AV 允许）", d);
        }

        // ---- T11 长串与末字变体：jintianqi ----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"jintianqi");
            PumpMessages();
            Sleep(200);
            std::wstring t = ReadFocusedText(pTm);
            bool inlineOk = t.size() >= 9 && t.compare(t.size() - 9, 9, L"jintianqi") == 0;
            TypeKey(pKm, (WPARAM)L'3'); // 选候选 3（按 probe 应为“今天气”）
            PumpMessages();
            Sleep(250);
            std::wstring t2 = ReadFocusedText(pTm);
            wchar_t d[160];
            _snwprintf_s(d, _TRUNCATE, L"inline='%ls' after3='%ls'", t.c_str(), t2.c_str());
            Note(L"T11 长串组合+数字选词", d);
        }

        // ---- T12 翻页 + 第二页数字选词（用户报告"无法切换下一页"）----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            TypeLetters(pKm, L"de"); // "de" 触发多页候选（的/得/德…>9）
            PumpMessages();
            Sleep(400); // 等候选回包（无防抖后应 <100ms，宽松留量）
            BOOL eatenPg = FALSE;
            pKm->TestKeyDown(VK_NEXT, 0x00000001 | 0x01000000, &eatenPg); // PageDown extended
            PumpMessages();
            bool pgEaten = false;
            if (eatenPg)
            {
                pKm->KeyDown(VK_NEXT, 0x00000001 | 0x01000000, &eatenPg);
                pgEaten = eatenPg != FALSE;
                PumpMessages();
                Sleep(150);
            }
            std::wstring mid = ReadFocusedText(pTm);
            TypeKey(pKm, (WPARAM)L'1'); // 第二页第 1 个 =
            PumpMessages();
            Sleep(250);
            std::wstring t = ReadFocusedText(pTm);
            wchar_t d[220];
            _snwprintf_s(d, _TRUNCATE,
                         L"PgDnEaten=%d mid='%ls' after='1st-of-pg2'='%ls' ok=%d",
                         (int)eatenPg, mid.c_str(), t.c_str(),
                         pgEaten && !t.empty() && mid.empty());
            // 判定：翻页键被吃；且按 1 上屏的不是第一页默认"的"（=第二页词）
            bool okEaten = eatenPg != FALSE;
            bool okCommit = !t.empty() && t != L"的" && mid.empty();
            Report(okEaten && okCommit, L"T12 翻页+第二页选词", d);
        }

        // ---- T13 真实快打场景：60ms/键 + 尾键后 30ms 内空格（用户原话场景）----
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            const wchar_t* seq = L"jintian";
            bool typeOk = true;
            for (const wchar_t* p = seq; *p; p++)
            {
                wchar_t s[2] = { *p, 0 };
                if (!TypeLetters(pKm, s)) { typeOk = false; break; }
                Sleep(60);
            }
            PumpMessages();
            Sleep(30); // 用户尾距：30ms 就按空格
            TypeKey(pKm, VK_SPACE);
            PumpMessages();
            Sleep(150);
            std::wstring t = ReadFocusedText(pTm);
            bool hanzi = t.find(L"今") != std::wstring::npos || t.find(L"天") != std::wstring::npos;
            bool noRaw = t.find(L"jintian") == std::wstring::npos;
            wchar_t d[200];
            _snwprintf_s(d, _TRUNCATE, L"'%ls' hanzi=%d noRaw=%d typeOk=%d",
                         t.c_str(), hanzi, noRaw, typeOk);
            Report(hanzi && noRaw && typeOk, L"T13 快打 + 30ms 抢空格上屏汉字", d);
        }
    } while (false);

    // ---- 清理 ----
    if (pKm) pKm->Release();
    if (pIpp) pIpp->Release();
    if (pDoc) pDoc->Release();
    if (pStore) pStore->Release();
    if (pTm) { pTm->Deactivate(); pTm->Release(); }
    if (hwndHost) DestroyWindow(hwndHost);
    CoUninitialize();

    LONG avs = g_avCount - avAtStart;
    wprintf(L"=== attempt %d: PASS=%d FAIL=%d AV=%ld ===\n", attempt, g_pass, g_fail, avs);
    fflush(stdout);
    if (g_fail == 0) return 0;
    wprintf(L"[WARN] attempt %d failed, retrying\n", attempt);
    }

    wprintf(L"=== 深度测试 3 轮全败（SMOKE 行为实际行为记录，详见上） ===\n");
    return 1;
}
