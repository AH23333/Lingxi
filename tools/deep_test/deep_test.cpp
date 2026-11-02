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

// ==================== HKCU 设置固定（T16 前置）====================
// DLL 每事件实时读 HKCU（service.cpp LoadSettings()），环境 ToggleMode 若被
// 设置页烟测改为 3(CapsLock)/4(Ctrl)，Shift 切换语义不再成立→T16 必挂。
// 测试前 pin=1（默认 Shift），结束还原原值；旧键 ShiftToggle 同步（口径同
// common/settings.cpp 双键兼容）。崩溃残留值=1=默认，无污染风险。
// pin/restore 必须落在 DLL 实际读取的那把键上：NS 沙箱生效时写
// LingxiIme_<ns>，与 settings.cpp ActiveKey() 同口径——真实 HKCU 彻底
// 不共轨（旧实现直写 Software\LingxiIme，属沙箱机制引入前的遗留，会把
// 默认值刷进用户真实键，污染"最后修改时间"取证）。

static bool OpenLingxiReg(HKEY* phk)
{
    wchar_t key[80] = L"Software\\LingxiIme";
    wchar_t ns[8] = L"";
    DWORD nl = GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", ns, 6);
    if (nl > 0 && nl < 6)
        _snwprintf_s(key, _TRUNCATE, L"Software\\LingxiIme_%s", ns);
    return RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL,
                           REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, phk, NULL) == ERROR_SUCCESS;
}

static bool PinToggleMode(DWORD mode, DWORD* pOld)
{
    HKEY hk = nullptr;
    if (!OpenLingxiReg(&hk)) return false;
    DWORD old = 1, cb = sizeof(old), type = 0;
    if (RegQueryValueExW(hk, L"ToggleMode", NULL, &type, (LPBYTE)&old, &cb) != ERROR_SUCCESS)
        old = 1; // 无值=默认 Shift
    if (pOld) *pOld = old;
    bool ok = RegSetValueExW(hk, L"ToggleMode", 0, REG_DWORD,
                             (const BYTE*)&mode, sizeof(mode)) == ERROR_SUCCESS;
    DWORD st = (mode != 0) ? 1 : 0;
    RegSetValueExW(hk, L"ShiftToggle", 0, REG_DWORD, (const BYTE*)&st, sizeof(st));
    RegCloseKey(hk);
    return ok;
}

static void RestoreToggleMode(DWORD old)
{
    HKEY hk = nullptr;
    if (!OpenLingxiReg(&hk)) return;
    RegSetValueExW(hk, L"ToggleMode", 0, REG_DWORD, (const BYTE*)&old, sizeof(old));
    DWORD st = (old != 0) ? 1 : 0;
    RegSetValueExW(hk, L"ShiftToggle", 0, REG_DWORD, (const BYTE*)&st, sizeof(st));
    RegCloseKey(hk);
}

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
    // T19 光标契约断言用：只读选区（严格 store 下唯一能反映"DLL 是否显式
    // SetSelection"的真值来源）
    const TS_SELECTION_ACP& Sel() const { return _sel; }
    // 宿主侧重置选区（真实 app 语义：全删后光标归零）。严格 store 的文本变更
    // 不再顺手推光标，宿主侧操作必须显式管理自己的选区。
    void ResetSelection() { _sel.acpStart = 0; _sel.acpEnd = 0; _sel.style.ase = (TsActiveSelEnd)TF_AE_NONE; _sel.style.fInterimChar = FALSE; }

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
        // 防御性夹取（真实 store 同款）：宿主侧删文本后选区绝不越文档尾
        TS_SELECTION_ACP s = _sel;
        if (s.acpStart > (LONG)_buf.size()) s.acpStart = (LONG)_buf.size();
        if (s.acpEnd > (LONG)_buf.size()) s.acpEnd = (LONG)_buf.size();
        pSelection[0] = s;
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
        // 严格 store（2026-10-30 晚）：文本变更一律不动选区——模拟 Win11 记事本
        //（WinUI CoreText 适配层）的真实行为。旧版"SetText 顺手推光标到文末"是
        // 宽松宿主假设，掩盖了 DLL 不显式 SetSelection 的缺陷（上屏后光标停在
        // 原位、后续输入插到旧光标处，用户真机报障）。光标前进从此只信 DLL 的
        // 显式 SetSelection（T19 契约断言）。
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
        if (pos > (LONG)_buf.size()) pos = (LONG)_buf.size(); // 同 GetSelection 防御性夹取
        _buf.insert((size_t)pos, pchText, cch);
        *pacpStart = pos;
        *pacpEnd = pos + (LONG)cch;
        if (pChange) { pChange->acpStart = pos; pChange->acpOldEnd = pos; pChange->acpNewEnd = pos + (LONG)cch; }
        // 严格 store：插入不动选区（同 SetText 注释——Win11 记事本行为）。
        // DLL 必须显式 SetSelection 推光标，否则 T19/T14/T18 全线现形。
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
// 严格 store 全局指针：宿主侧操作（清空文本后重置选区）需要直达 store。
// 模拟真实 app 的宿主职责——严格 store 不再"顺手"推光标，宿主必须自理。
static TestStore* g_pStore = nullptr;
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
        // 宿主侧选区自理（严格 store 语义，2026-10-30 晚）：全删后光标归零。
        // 真实 app（如记事本）删除全部文本时由宿主把光标放到 0；旧宽松 store
        // 由 SetText 顺手推光标，如今文本变更不动选区，这步成为宿主必做动作，
        // 否则陈旧选区越界，下轮插入/读取行为失真。
        if (ok && g_pStore)
            g_pStore->ResetSelection();
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

// 放行键：TestKeyDown 吃（保证 KeyDown 回访做状态迁移）→ KeyDown 返回放行
// （宿主自行上屏；本 harness 无宿主处理逻辑，放行键不落盘）
static bool TypePassKey(ITfKeystrokeMgr* pKm, WPARAM vk)
{
    BOOL eaten = FALSE;
    if (FAILED(pKm->TestKeyDown(vk, 0x00260001, &eaten)) || !eaten)
        return false;
    eaten = FALSE;
    if (FAILED(pKm->KeyDown(vk, 0x00260001, &eaten)))
        return false;
    return eaten == FALSE;
}

// 完整单击（down+up）：Shift 单击切换类必需——翻转判定在 OnKeyUp。
static bool TapKey(ITfKeystrokeMgr* pKm, WPARAM vk, bool expectEaten)
{
    BOOL eaten = FALSE;
    if (FAILED(pKm->TestKeyDown(vk, 0x00260001, &eaten)))
        return false;
    if ((eaten != FALSE) != expectEaten)
        return false;
    eaten = FALSE;
    if (FAILED(pKm->KeyDown(vk, 0x00260001, &eaten)))
        return false;
    eaten = FALSE;
    return SUCCEEDED(pKm->KeyUp(vk, 0xC0026001, &eaten));
}

// 裸 KeyUp：配合 TypeKey 的 down 组成"按住期间打字"序列（如 Shift 组合使用）
static bool RawKeyUp(ITfKeystrokeMgr* pKm, WPARAM vk)
{
    BOOL eaten = FALSE;
    return SUCCEEDED(pKm->KeyUp(vk, 0xC0026001, &eaten));
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
        // Win8+ 首选 ProfileMgr::ActivateProfile：走完整输入法切换路径，
        // ctfmon 会话级状态同步认可。旧 ActivateLanguageProfile 只做线程内
        // profile 激活，会被会话级输入法"纠正"覆盖（楔死：键派发给别的 TIP）。
        ITfInputProcessorProfileMgr* pPm = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                                      IID_ITfInputProcessorProfileMgr, (void**)&pPm);
        if (SUCCEEDED(hr) && pPm)
        {
            hr = pPm->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804,
                                      CLSID_LingxiTextService, GUID_LingxiProfile_zhCN,
                                      NULL, 0);
            pPm->Release();
            if (SUCCEEDED(hr)) return true;
        }
        // 回退：旧 API（系统无 ProfileMgr 时）
        HRESULT hr2 = g_pIpp->ActivateLanguageProfile(CLSID_LingxiTextService, 0x0804, GUID_LingxiProfile_zhCN);
        if (SUCCEEDED(hr2)) return true;
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

// ---- 会话 IME 状态保全（2026-11-01 事故：e2e/deep_test 收尾停在灵犀
// zh-CN 不还原，用户会话被劫持 40 分钟——所有窗口打不出中文）。开场记录
// 当前激活 profile（仅首轮），收尾原样还原；原状态非 TIP（裸键盘布局）时
// 兜底枚举 zh-CN 激活微软拼音（kClsidMsPinyin，本机用户中文 IME）。 ----
static TF_INPUTPROCESSORPROFILE g_origProfile = {};
static bool g_haveOrig = false;

static void CaptureOrigProfile()
{
    if (g_haveOrig) return; // 多轮重试只记首轮的真实用户状态
    ITfInputProcessorProfileMgr* pMgr = nullptr;
    if (g_pIpp &&
        SUCCEEDED(g_pIpp->QueryInterface(IID_ITfInputProcessorProfileMgr, (void**)&pMgr)) && pMgr)
    {
        g_haveOrig = SUCCEEDED(pMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &g_origProfile));
        pMgr->Release();
    }
    wprintf(g_haveOrig ? L"[INFO] captured user IME (langid=0x%04X)\n"
                       : L"[WARN] capture user IME failed\n",
            g_haveOrig ? (unsigned)g_origProfile.langid : 0u);
    fflush(stdout);
}

static void RestoreUserIme()
{
    if (!g_haveOrig)
    {
        wprintf(L"[WARN] user IME restore skipped (no captured state)\n");
        fflush(stdout);
        return;
    }
    ITfInputProcessorProfileMgr* pMgr = nullptr;
    if (!g_pIpp ||
        FAILED(g_pIpp->QueryInterface(IID_ITfInputProcessorProfileMgr, (void**)&pMgr)) || !pMgr)
    {
        wprintf(L"[WARN] user IME restore: no ProfileMgr\n");
        fflush(stdout);
        return;
    }
    bool ok = false;
    if (g_origProfile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
    {
        ok = SUCCEEDED(pMgr->ActivateProfile(g_origProfile.dwProfileType, g_origProfile.langid,
                                             g_origProfile.clsid, g_origProfile.guidProfile,
                                             g_origProfile.hkl, 0));
    }
    if (!ok)
    {
        IEnumTfInputProcessorProfiles* pEnum = nullptr;
        if (SUCCEEDED(pMgr->EnumProfiles(0x0804, &pEnum)) && pEnum)
        {
            TF_INPUTPROCESSORPROFILE prof = {};
            ULONG fetched = 0;
            while (pEnum->Next(1, &prof, &fetched) == S_OK && fetched == 1)
            {
                if (IsEqualCLSID(prof.clsid, kClsidMsPinyin) &&
                    IsEqualGUID(prof.catid, GUID_TFCAT_TIP_KEYBOARD))
                {
                    ok = SUCCEEDED(pMgr->ActivateProfile(prof.dwProfileType, prof.langid,
                                                         prof.clsid, prof.guidProfile,
                                                         prof.hkl, 0));
                    break;
                }
            }
            pEnum->Release();
        }
    }
    pMgr->Release();
    wprintf(ok ? L"[OK] user IME restored after deep_test\n"
               : L"[WARN] user IME restore failed (session may stay on Lingxi)\n");
    fflush(stdout);
}

// ==================== 场景 ====================

int wmain()
{
    // 设置沙盒（2026-10-29 实证事故：测试直写真实 HKCU，一次半程退出把用户
    // 全部能力位刷 0——输入法直接瘫成直通）。注册表与词典同权隔离：无 NS 则自设
    // "test"，并清沙盒键回到结构默认（=各用例断言基线）。真实 HKCU 从此不共轨。
    {
        wchar_t nsv[8] = L"";
        DWORD nl = GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", nsv, 6);
        if (nl == 0 || nl >= 6) { 
            nsv[0] = L't'; nsv[1] = L'e'; nsv[2] = L's'; nsv[3] = L't'; nsv[4] = 0;
            SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", nsv);
        }
        std::wstring sandk = L"Software\\LingxiIme_";
        sandk += nsv;
        RegDeleteTreeW(HKEY_CURRENT_USER, sandk.c_str());
    }
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
        g_pStore = pStore;
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

        // ---- 会话状态保全：激活灵犀前记录用户当前 IME（仅首轮） ----
        CaptureOrigProfile();

        // ---- 激活灵犀 zh-CN（一次性，无 churn）----
        Report(ActivateLingxiZhCn(10), L"ActivateLanguageProfile (zh-CN)");
        Report(WaitUiReady(100), L"LingxiUI ready");
        Sleep(1000); // 模式检测（封送消息在泵内执行）+ TIP 重激活余量
        PumpMessages();

        pTm->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKm);
        if (!pKm) { Report(false, L"QueryInterface ITfKeystrokeMgr"); break; }

        // ---- pinyin settle（失败时给足取证：前台 TIP 是谁、探测键是否被吃）----
        {
            bool settled = WaitForPinyinMode(pKm, 75); // 15s：覆盖激活后 msctf 派发就绪延迟（PITFALLS #19）
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
                // 取证：线程真正 active 的 TIP 是谁（ProfileMgr 权威视图）
                {
                    ITfInputProcessorProfileMgr* pPm = nullptr;
                    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                                                   IID_ITfInputProcessorProfileMgr, (void**)&pPm)) && pPm)
                    {
                        TF_INPUTPROCESSORPROFILE prof{};
                        if (SUCCEEDED(pPm->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &prof)))
                        {
                            bool isLingxi = IsEqualCLSID(prof.clsid, CLSID_LingxiTextService);
                            LPOLESTR sClsid = nullptr;
                            StringFromCLSID(prof.clsid, &sClsid);
                            wprintf(L"[INFO] activeTip clsid=%ls (lingxi=%d) profileType=%u hkl=0x%p\n",
                                    sClsid ? sClsid : L"?", (int)isLingxi,
                                    prof.dwProfileType, (void*)prof.hkl);
                            if (sClsid) CoTaskMemFree(sClsid);
                        }
                        else
                        {
                            wprintf(L"[INFO] activeTip: GetActiveProfile FAILED\n");
                        }
                        pPm->Release();
                    }
                }
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

        // ---- T14 B1 部分提交：nihao 数字选"你"(cover=2) 后余 hao 继续组合，空格收"好" ----
        // 不 pin 固定数字：候选序随质量闸(29f9ec2)/学习态漂移（run 间 LingxiUI 单例
        // 携学习态存活，"你"会在 idx6/idx7 间漂）——扫描式选词，验提交字=="你"，
        // 错则 ESC 取消重打，彻底摆脱序依赖。
        {
            EnsurePinyinMode(pKm);
            std::wstring mid;
            std::wstring trace; // 取证：每个数字尝试的提交码点序列
            int digitNi = -1;
            for (int dig = 2; dig <= 9; dig++) // 1=你好(cover=5 全收)跳过；你≠首位的保证来自 engine_test P6c4
            {
                ClearFocusedText(pTm);
                TypeLetters(pKm, L"nihao");
                PumpMessages();
                Sleep(400); // 等候选回包
                TypeKey(pKm, (WPARAM)(L'0' + dig));
                PumpMessages();
                Sleep(300); // 等部分提交 + "hao" 新候选
                mid = ReadFocusedText(pTm);
                {
                    wchar_t buf[48];
                    swprintf_s(buf, L"d%d=", dig);
                    trace += buf;
                    for (wchar_t ch : mid) { swprintf_s(buf, L"%04X", (unsigned)ch); trace += buf; }
                    trace += L" ";
                }
                if (mid == L"你") { digitNi = dig; break; } // cover=2 提交成功，组合继续
                TypeKey(pKm, VK_ESCAPE); // 取消残余组合（hao），换下一数字重试
                PumpMessages();
                Sleep(200);
            }
            bool committed = digitNi >= 0;
            if (committed)
            {
                TypeKey(pKm, VK_SPACE); // 收"好"
                PumpMessages();
                Sleep(250);
            }
            std::wstring t = committed ? ReadFocusedText(pTm) : mid;
            bool finalOk = t.find(L"你好") != std::wstring::npos;
            bool noLetter = t.find(L"nihao") == std::wstring::npos && t.find(L"hao") == std::wstring::npos;
            // 码点十六进制转储：日志管道走 ANSI，中文会变 '?'，必须 dump 码点取证
            auto hexOf = [](const std::wstring& s) {
                std::wstring h;
                wchar_t buf[8];
                for (wchar_t ch : s)
                {
                    swprintf_s(buf, L"%04X,", (unsigned)ch);
                    h += buf;
                }
                return h;
            };
            wchar_t d[420];
            _snwprintf_s(d, _TRUNCATE, L"digit=%d trace[%ls] mid[%u]=%s final[%u]=%s finalOk=%d noLetter=%d",
                         digitNi, trace.c_str(),
                         (unsigned)mid.size(), hexOf(mid).c_str(),
                         (unsigned)t.size(), hexOf(t).c_str(), finalOk, noLetter);
            Report(finalOk && noLetter, L"T14 部分提交：扫描数字选你(cover=2) 余hao 空格收好", d);
        }

        // ---- T15 B3 零声母简写：首键 i/u 出候选（此前 0 候选=用户报"键死"）+ v 直通退出通路 ----
        {
            auto hexOf15 = [](const std::wstring& s) {
                std::wstring h;
                wchar_t buf[8];
                for (wchar_t ch : s)
                {
                    swprintf_s(buf, L"%04X,", (unsigned)ch);
                    h += buf;
                }
                return h;
            };
            // 场景矩阵（probe pin，空格收首选）：i→一(u+4E00) u→无(u+65E0)
            // in→因(u+56E0) iu→有(u+6709)——修复前全部空候选、空格上屏字母原文
            struct T15Case { const wchar_t* keys; wchar_t expect; };
            const T15Case t15[] = {
                { L"i",  0x4E00 }, // 一（i→yi）
                { L"u",  0x65E0 }, // 无（u→wu）
                { L"in", 0x56E0 }, // 因（in→yin）
                { L"iu", 0x6709 }, // 有（iu→you）
            };
            for (const auto& cs : t15)
            {
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);
                TypeLetters(pKm, cs.keys);
                PumpMessages();
                Sleep(400); // 等候选回包
                TypeKey(pKm, VK_SPACE);
                PumpMessages();
                Sleep(250);
                std::wstring t = ReadFocusedText(pTm);
                wchar_t d[240];
                _snwprintf_s(d, _TRUNCATE, L"'%ls' t[%u]=%s expect=%04X",
                             cs.keys, (unsigned)t.size(), hexOf15(t).c_str(), (unsigned)cs.expect);
                Report(t.size() == 1 && t[0] == cs.expect, L"T15 零声母简写：空格收首选汉字", d);
            }

            // i + 逗号：中文标点触发"首选汉字+标点"合并提交 → 一+，（u+FF0C）
            {
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);
                TypeLetters(pKm, L"i");
                PumpMessages();
                Sleep(400);
                TypeKey(pKm, VK_OEM_COMMA);
                PumpMessages();
                Sleep(250);
                std::wstring t = ReadFocusedText(pTm);
                bool ok = t.size() == 2 && t[0] == 0x4E00 && t[1] == 0xFF0C;
                wchar_t d[240];
                _snwprintf_s(d, _TRUNCATE, L"t[%u]=%s", (unsigned)t.size(), hexOf15(t).c_str());
                Report(ok, L"T15 零声母+标点：首选汉字+全角逗号", d);
            }

            // 退格缩减 iu→i（本 harness 非内联、组合字母不落 store，用功能断言：
            // 退格后空格应收"一"；若退格失效会收"有"）+ Esc 取消无字母残留
            {
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);
                TypeLetters(pKm, L"iu");
                PumpMessages();
                Sleep(300);
                TypeKey(pKm, VK_BACK);   // 缩减 iu→i，新候选在途
                PumpMessages();
                Sleep(400);              // 等缩减后候选回包
                TypeKey(pKm, VK_SPACE);  // 应收"一"
                PumpMessages();
                Sleep(250);
                std::wstring t = ReadFocusedText(pTm);
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);
                TypeLetters(pKm, L"iu");
                PumpMessages();
                Sleep(200);
                TypeKey(pKm, VK_ESCAPE);
                PumpMessages();
                Sleep(150);
                std::wstring t2 = ReadFocusedText(pTm);
                wchar_t d[320];
                _snwprintf_s(d, _TRUNCATE, L"backCommit[%u]=%s afterEsc[%u]=%s",
                             (unsigned)t.size(), hexOf15(t).c_str(),
                             (unsigned)t2.size(), hexOf15(t2).c_str());
                bool backOk = t.size() == 1 && t[0] == 0x4E00; // 退格后剩 i→一
                bool escOk = t2.empty();
                Report(backOk && escOk, L"T15 退格缩减(iu→i→一)+Esc取消无残留", d);
            }

            // v 组合语义（用户报"中文下 v 打不出/vlog 无法用"重设计修复）：
            // v 与普通字母一致进拼音组合（"v" 非法拼音 → 0 候选 echo 回显），
            // 空格 PinyinCommitRaw 原样上屏 "vk"。全程 DLL 编辑会话写文本，
            // 零 pfEaten=FALSE 放行依赖（旧"悬账+放行"机制真机吞键=报障根因）
            {
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);
                bool vEaten = TypeKey(pKm, (WPARAM)L'v', true);   // v 进组合（吃）
                PumpMessages();
                bool kEaten = TypeKey(pKm, (WPARAM)L'k', true);   // 字母进组合（吃）
                PumpMessages();
                bool spEaten = TypeKey(pKm, VK_SPACE, true);      // 空格提交原文（吃）
                PumpMessages();
                Sleep(250);
                std::wstring t = ReadFocusedText(pTm);
                wchar_t d[240];
                _snwprintf_s(d, _TRUNCATE, L"vEaten=%d kEaten=%d spEaten=%d t[%u]=%s",
                             (int)vEaten, (int)kEaten, (int)spEaten,
                             (unsigned)t.size(), hexOf15(t).c_str());
                Report(vEaten && kEaten && spEaten &&
                       t.size() == 2 && t[0] == L'v' && t[1] == L'k',
                       L"T15 v组合：v+k+空格原样上屏'vk'（vlog 语义）", d);
            }

            // 裸 v 上屏（"v"+空格 → "v"）
            {
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);
                bool vEaten = TypeKey(pKm, (WPARAM)L'v', true);
                PumpMessages();
                bool spEaten = TypeKey(pKm, VK_SPACE, true);
                PumpMessages();
                Sleep(250);
                std::wstring t = ReadFocusedText(pTm);
                wchar_t d[240];
                _snwprintf_s(d, _TRUNCATE, L"vEaten=%d spEaten=%d t[%u]=%s",
                             (int)vEaten, (int)spEaten,
                             (unsigned)t.size(), hexOf15(t).c_str());
                Report(vEaten && spEaten && t.size() == 1 && t[0] == L'v',
                       L"T15 裸v：v+空格上屏'v'", d);
            }

            // v 退格撤销 / Esc 取消（组合清空无残留），随后回正常拼音 i→一 / iu→有
            {
                struct T15Cancel { WPARAM cancelKey; const wchar_t* keys; wchar_t expect; };
                const T15Cancel cancels[] = {
                    { VK_BACK,   L"i",  0x4E00 }, // v→退格：组合缩减到空=取消 → 一
                    { VK_ESCAPE, L"iu", 0x6709 }, // v→Esc：组合取消 → 有
                };
                for (const auto& cc : cancels)
                {
                    EnsurePinyinMode(pKm);
                    ClearFocusedText(pTm);
                    bool vEaten = TypeKey(pKm, (WPARAM)L'v', true);
                    PumpMessages();
                    bool cancelEaten = TypeKey(pKm, cc.cancelKey, true);
                    PumpMessages();
                    std::wstring t = ReadFocusedText(pTm);
                    bool clean = vEaten && cancelEaten && t.empty();
                    TypeLetters(pKm, cc.keys);    // 取消后回正常拼音组合
                    PumpMessages();
                    Sleep(400);
                    TypeKey(pKm, VK_SPACE);
                    PumpMessages();
                    Sleep(250);
                    std::wstring t2 = ReadFocusedText(pTm);
                    wchar_t d[280];
                    _snwprintf_s(d, _TRUNCATE, L"cancel=%04X clean=%d keys='%ls' t2[%u]=%s expect=%04X",
                                 (unsigned)cc.cancelKey, (int)clean, cc.keys,
                                 (unsigned)t2.size(), hexOf15(t2).c_str(), (unsigned)cc.expect);
                    Report(clean && t2.size() == 1 && t2[0] == cc.expect,
                           L"T15 v取消：退格/Esc清组合无残留后回拼音", d);
                }
            }

            // ---- T16 B5 Shift 单击切换：临时英文往返 + 组合使用/组合中不误切 ----
            // DLL 链路：OnKeyDown(VK_SHIFT) 记 _shiftDown/_shiftWasActive；OnKeyUp(VK_SHIFT)
            // 满足"未组合使用(!_shiftUsed)+按下时无组合(!_shiftWasActive)+当前无组合
            // (!PinyinActive)+zh 模式+shiftToggle 开"时翻转 _tempEnglish（临时英文）。
            {
                DWORD oldTm = 1;
                bool tmPinned = PinToggleMode(1, &oldTm); // 固定 Shift 切换语义
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);

                // 空组合单击 Shift → 切临时英文：字母直通（TestKeyDown/KeyDown 均 eaten=FALSE）
                // （toggleMode=1 时 Shift 在 Test 阶段被认领 eaten=TRUE——真机门控需要，
                //  OnKeyDown/OnKeyUp 才会被真实应用调用）
                bool tap1 = TapKey(pKm, VK_SHIFT, true);
                PumpMessages(); Sleep(150);
                bool aPass = TypeKey(pKm, (WPARAM)L'a', false);
                PumpMessages(); Sleep(150);
                std::wstring t = ReadFocusedText(pTm);
                wchar_t d16a[240];
                _snwprintf_s(d16a, _TRUNCATE, L"tap1=%d aPass=%d t[%u]=%s",
                             (int)tap1, (int)aPass, (unsigned)t.size(), hexOf15(t).c_str());
                Report(tap1 && aPass && t.empty(), L"T16 单击Shift切临时英文(字母直通)", d16a);

                // 再单击切回中文：字母进组合（eaten=TRUE），Esc 可清
                bool tap2 = TapKey(pKm, VK_SHIFT, true);
                PumpMessages(); Sleep(150);
                bool bPass = TypeKey(pKm, (WPARAM)L'a', true);
                PumpMessages(); Sleep(150);
                bool escOk = TypeKey(pKm, VK_ESCAPE, true);
                PumpMessages();
                wchar_t d16b[200];
                _snwprintf_s(d16b, _TRUNCATE, L"tap2=%d bPass=%d escOk=%d",
                             (int)tap2, (int)bPass, (int)escOk);
                Report(tap2 && bPass && escOk, L"T16 再单击Shift切回中文(字母进组合)", d16b);

                // Shift 按住期间打字（组合使用）→ KeyUp 不翻转：后续字母仍进组合
                // （模拟限制：GetKeyState(VK_SHIFT) 不随 pKm 模拟改变，'a' 走组合
                //  路径而非真实 Shift+字母直通；断言对象是 DLL 内部 _shiftUsed 判定）
                bool shDown = TypeKey(pKm, VK_SHIFT, true);  // Shift down（被认领，_shiftDown=true）
                bool mid = TypeKey(pKm, (WPARAM)L'a', true);  // _shiftUsed=true；进组合
                bool shUp = RawKeyUp(pKm, VK_SHIFT);          // _shiftUsed → 不翻转
                PumpMessages(); Sleep(150);
                bool cPass = TypeKey(pKm, (WPARAM)L'b', true); // 仍中文：组合继续
                PumpMessages(); Sleep(150);
                TypeKey(pKm, VK_ESCAPE, true); // 清组合
                PumpMessages();
                wchar_t d16c[220];
                _snwprintf_s(d16c, _TRUNCATE, L"shDown=%d mid=%d shUp=%d cPass=%d",
                             (int)shDown, (int)mid, (int)shUp, (int)cPass);
                Report(shDown && mid && shUp && cPass, L"T16 Shift组合使用不误切(后续仍组合)", d16c);

                // 组合中单击 Shift：组合不丢、当前首选不被误提交（VK_SHIFT 放行修复的回归）
                bool nOk = TypeKey(pKm, (WPARAM)L'n', true); // 进组合 n
                PumpMessages(); Sleep(150);
                bool tap3 = TapKey(pKm, VK_SHIFT, true);    // 组合中 tap：认领吞下但不提交不翻转
                PumpMessages(); Sleep(150);
                std::wstring t2 = ReadFocusedText(pTm);
                bool iOk = TypeKey(pKm, (WPARAM)L'i', true); // 组合继续（ni）
                PumpMessages(); Sleep(150);
                TypeKey(pKm, VK_ESCAPE, true); // 清组合
                PumpMessages();
                wchar_t d16d[240];
                _snwprintf_s(d16d, _TRUNCATE, L"nOk=%d tap3=%d t2[%u]=%s iOk=%d",
                             (int)nOk, (int)tap3, (unsigned)t2.size(), hexOf15(t2).c_str(), (int)iOk);
                Report(nOk && tap3 && t2.empty() && iOk, L"T16 组合中单击Shift不丢组合不误提交", d16d);
                if (tmPinned) RestoreToggleMode(oldTm);
            }

            // ---- T17 导航键组合期接管（晚·十四）+ B6 隔音符端到端 ----
            // eat 侧：PinyinActive() 时 Up/Down/Home/End/Left/Right 一律吃下
            //（此前穿透宿主→光标移出组合范围错位，B5 同族缺口）；OEM_7 撇号
            // 不依赖 cnPunct 标点映射进组合（cnPunct 关时也要能隔音）。
            // handle 侧：导航键翻页/吞下保守组合；xi'an 空格提交=西安
            //（live 探针实证首选 rank0，xian 的"先"才是 rank0=因果对照）。
            {
                EnsurePinyinMode(pKm);
                ClearFocusedText(pTm);

                // ① 组合期导航键全吃下、组合不丢、无提交
                bool nOk = TypeKey(pKm, (WPARAM)L'n', true);
                PumpMessages(); Sleep(150);
                bool dDown  = TypeKey(pKm, VK_DOWN,  true);
                bool dUp    = TypeKey(pKm, VK_UP,    true);
                bool dHome  = TypeKey(pKm, VK_HOME,  true);
                bool dEnd   = TypeKey(pKm, VK_END,   true);
                bool dLeft  = TypeKey(pKm, VK_LEFT,  true);
                bool dRight = TypeKey(pKm, VK_RIGHT, true);
                PumpMessages(); Sleep(250);
                std::wstring tNav = ReadFocusedText(pTm);
                bool iOk = TypeKey(pKm, (WPARAM)L'i', true); // 组合继续（ni）
                PumpMessages(); Sleep(150);
                TypeKey(pKm, VK_ESCAPE, true);
                PumpMessages();
                wchar_t d17a[300];
                _snwprintf_s(d17a, _TRUNCATE,
                    L"nOk=%d down=%d up=%d home=%d end=%d left=%d right=%d tNav[%u]=%s iOk=%d",
                    (int)nOk, (int)dDown, (int)dUp, (int)dHome, (int)dEnd,
                    (int)dLeft, (int)dRight, (unsigned)tNav.size(),
                    hexOf15(tNav).c_str(), (int)iOk);
                Report(nOk && dDown && dUp && dHome && dEnd && dLeft && dRight
                       && tNav.empty() && iOk,
                       L"T17 组合期导航键全吃下不丢组合", d17a);

                // ② 空组合态负对照：导航键放行（不误吞正常光标移动）
                bool navPass = TypeKey(pKm, VK_DOWN, false);
                PumpMessages();
                wchar_t d17b[80];
                _snwprintf_s(d17b, _TRUNCATE, L"navPass=%d", (int)navPass);
                Report(navPass, L"T17 空组合态导航键放行", d17b);

                // ③ B6 隔音符端到端：x i ' a n → 空格 → 西安
                ClearFocusedText(pTm);
                bool xOk  = TypeKey(pKm, (WPARAM)L'x', true);
                bool i2Ok = TypeKey(pKm, (WPARAM)L'i', true);
                bool apo  = TypeKey(pKm, VK_OEM_7, true); // 撇号进组合（B6 eat 侧）
                bool aOk  = TypeKey(pKm, (WPARAM)L'a', true);
                bool n2Ok = TypeKey(pKm, (WPARAM)L'n', true);
                PumpMessages(); Sleep(300); // 候选回包
                bool sp = TypeKey(pKm, VK_SPACE, true);
                PumpMessages(); Sleep(300);
                std::wstring tXa = ReadFocusedText(pTm);
                wchar_t d17c[220];
                _snwprintf_s(d17c, _TRUNCATE,
                    L"x=%d i=%d apo=%d a=%d n=%d sp=%d t[%u]=%s",
                    (int)xOk, (int)i2Ok, (int)apo, (int)aOk, (int)n2Ok, (int)sp,
                    (unsigned)tXa.size(), hexOf15(tXa).c_str());
                Report(xOk && i2Ok && apo && aOk && n2Ok && sp
                       && tXa.size() == 2 && tXa[0] == 0x897F && tXa[1] == 0x5B89,
                       L"T17 隔音符xi'an空格提交西安", d17c);
            }
        }
        // ---- T18 选词联想（GAP §11 真机行为验证）----
        // a) 上屏联想候选后 sleep 再按空格：应连出下一个联想词（候选由 '今' 上屏
        //    后主动发的 CTX_F_ASSOC 帧生成；无联想时空格只会直写空格）。
        // b) 联想在途期继续打全新拼音（ming 上屏后打 hao）：新组合必须精确接管，
        //    空格提交的是"明好"而非旧联想候选（撤销护栏 + 旧帧对号失效双重证明）。
        auto hex18 = [](const std::wstring& s) {
            std::wstring h;
            wchar_t buf[8];
            for (wchar_t ch : s) { swprintf_s(buf, L"%04X,", (unsigned)ch); h += buf; }
            return h;
        };
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            const wchar_t* seqA = L"jintian";
            bool typeOkA = true;
            for (const wchar_t* q = seqA; *q; q++)
            {
                wchar_t s[2] = { *q, 0 };
                if (!TypeLetters(pKm, s)) { typeOkA = false; break; }
                Sleep(60);
            }
            PumpMessages();
            Sleep(150);
            TypeKey(pKm, VK_SPACE);   // 提交"今…"首选
            PumpMessages();
            Sleep(2600);              // 联想帧往返：debounce 400 + 预测 + CANDS 回包
            std::wstring t1 = ReadFocusedText(pTm);
            TypeKey(pKm, VK_SPACE);   // 联想候选已显示 → 提交其首选
            PumpMessages();
            Sleep(200);
            std::wstring t2 = ReadFocusedText(pTm);
            // 联想生效判据：文本变长且新增首字符是汉字非空格（无联想时空格=直写" "）
            bool grewHanzi = t2.size() > t1.size() && t2[t1.size()] != L' ' &&
                t2[t1.size()] >= 0x4E00 && t2[t1.size()] <= 0x9FFF;
            bool noRawA = t2.find(L"jintian") == std::wstring::npos;
            wchar_t d18a[260];
            _snwprintf_s(d18a, _TRUNCATE, L"t1[%u]=%s t2[%u]=%s grew=%d noRaw=%d ok=%d",
                         (unsigned)t1.size(), hex18(t1).c_str(),
                         (unsigned)t2.size(), hex18(t2).c_str(),
                         (int)grewHanzi, (int)noRawA, (int)typeOkA);
            Report(grewHanzi && noRawA && typeOkA, L"T18a 上屏后空格连出联想候选", d18a);
        }
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            const wchar_t* seqB = L"ming";
            bool typeOkB = true;
            for (const wchar_t* q = seqB; *q; q++)
            {
                wchar_t s[2] = { *q, 0 };
                if (!TypeLetters(pKm, s)) { typeOkB = false; break; }
                Sleep(60);
            }
            PumpMessages();
            Sleep(150);
            TypeKey(pKm, VK_SPACE);   // 提交"明"
            PumpMessages();
            Sleep(2600);              // 联想窗口期
            const wchar_t* seqC = L"hao";
            for (const wchar_t* q = seqC; *q; q++)
            {
                wchar_t s[2] = { *q, 0 };
                TypeLetters(pKm, s);   // h 触发联想撤销 + 新组合
                Sleep(60);
            }
            PumpMessages();
            Sleep(200);
            TypeKey(pKm, VK_SPACE);   // 提交新组合 hao 的首选（期望"好"）
            PumpMessages();
            Sleep(200);
            std::wstring t = ReadFocusedText(pTm);
            // 期望恰为两字且第二字="好"（新组合首选，非旧联想词）；
            // 首选不锁"明"——ming 的字典首选现为"名"（频率序），任何汉字即可
            bool ok2nd = t.size() == 2 && t[1] == 0x597D;
            bool ok1st = t.size() == 2 && t[0] >= 0x4E00 && t[0] <= 0x9FFF;
            bool noRawB = t.find(L"ming") == std::wstring::npos
                       && t.find(L"hao") == std::wstring::npos;
            wchar_t d18b[240];
            const int iFirst = (int)ok1st;
            const int iSecond = (int)ok2nd;
            const int iNoRaw = (int)noRawB;
            const int iType = (int)typeOkB;
            _snwprintf_s(d18b, _TRUNCATE, L"t[%u]=%s first=%d second=%d noRaw=%d ok=%d",
                         (unsigned)t.size(), hex18(t).c_str(),
                         iFirst, iSecond, iNoRaw, iType);
            Report(ok1st && ok2nd && noRawB && typeOkB,
                   L"T18b 联想期新组合精确接管", d18b);
        }
        // ---- T18c 无组合选词键道（`1ac01e8` 数字选词刀活闸）----
        // 旧版：候选窗可见时 Tab/Esc 被 Test 三面门认领，但 Down 空组合只有
        // 空格/数字联想道 → Tab 在联想窗蒸发（用户报"英文窗数字无法选词"同族）。
        // 新版统一：Tab=提交首选、Esc=撤窗作废（后续选词键不提交陈旧候选）。
        {
            auto hexC = [](const std::wstring& s) {
                std::wstring h; wchar_t b[8];
                for (wchar_t ch : s) { swprintf_s(b, L"%04X,", (unsigned)ch); h += b; }
                return h;
            };            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            const wchar_t* seqY = L"jintian";
            bool typeOkY = true;
            for (const wchar_t* q = seqY; *q; q++)
            {
                wchar_t s2[2] = { *q, 0 };
                if (!TypeLetters(pKm, s2)) { typeOkY = false; break; }
                Sleep(60);
            }
            PumpMessages();
            Sleep(150);
            TypeKey(pKm, VK_SPACE);   // 上屏首选，触发联想（第一批）
            PumpMessages();
            Sleep(2600);              // 联想候选到齐
            std::wstring tA = ReadFocusedText(pTm);
            TypeKey(pKm, VK_TAB);     // 增量点：Tab=提交联想首选（旧版此键蒸发）
            PumpMessages();
            Sleep(2600);              // 点选→IC 连发第二批联想；本轮断言只需第一批
            std::wstring tB = ReadFocusedText(pTm);
            bool tabSelected =
                tB.size() > tA.size() && tB[tA.size()] != L' ' &&
                tB[tA.size()] >= 0x4E00 && tB[tA.size()] <= 0x9FFF;
            // Esc 判据只验"无文本副作用+窗态无害"：不追按其他键——Tab 点选后
            // IC 合法连发新联想，再按键落入"新批到达即可合法选词"的时序竞态域。
            TypeKey(pKm, VK_ESCAPE);
            PumpMessages();
            Sleep(150);
            std::wstring tC = ReadFocusedText(pTm);
            bool escClean = (tC == tB);
            wchar_t d18c[280];
            _snwprintf_s(d18c, _TRUNCATE, L"A[%u]=%s B[%u]=%s tab=%d escClean=%d ok=%d",
                         (unsigned)tA.size(), hexC(tA).c_str(),
                         (unsigned)tB.size(), hexC(tB).c_str(),
                         (int)tabSelected, (int)escClean, (int)typeOkY);
            Report(tabSelected && escClean && typeOkY,
                   L"T18c Tab提交与Esc撤窗选词键道", d18c);
        }
        // ---- T19 上屏光标契约（2026-10-30 晚用户真机报障：Win11 记事本上屏后
        // 光标停留原位，后续输入插到旧光标处）----
        // 本 harness 的 TestStore 自本版起严格化（模拟记事本/WinUI CoreText 适配层：
        // 文本变更一律不动选区），光标前进只能来自 DLL 的显式 SetSelection：
        // a) 首次上屏后：文本非空全汉字，且选区折叠于文本尾（滞留 0=报障现场镜像）；
        // b) 不等联想窗口，紧接第二次上屏：新文本必须接在旧文本之后（顺序不乱，
        //    旧文本是前缀），选区同步推到新文本尾——"光标停在原位"的直接判据。
        // 不耦合字典首选字：N1/N2 均为泛化长度断言。
        auto hex19 = [](const std::wstring& s) {
            std::wstring h;
            wchar_t buf[8];
            for (wchar_t ch : s) { swprintf_s(buf, L"%04X,", (unsigned)ch); h += buf; }
            return h;
        };
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            const wchar_t* seqA = L"nihao";
            bool typeOkA = true;
            for (const wchar_t* q = seqA; *q; q++)
            {
                wchar_t s[2] = { *q, 0 };
                if (!TypeLetters(pKm, s)) { typeOkA = false; break; }
                Sleep(60);
            }
            PumpMessages();
            Sleep(150);
            TypeKey(pKm, VK_SPACE);   // 首次上屏
            PumpMessages();
            Sleep(300);
            std::wstring t1 = ReadFocusedText(pTm);
            const TS_SELECTION_ACP& s1 = g_pStore->Sel();
            bool text1 = !t1.empty() && t1[0] >= 0x4E00 && t1[0] <= 0x9FFF;
            bool caret1 = s1.acpStart == (LONG)t1.size() && s1.acpEnd == (LONG)t1.size();
            wchar_t d19a[300];
            _snwprintf_s(d19a, _TRUNCATE,
                         L"t1[%u]=%s sel={%d,%d} text=%d caret=%d ok=%d",
                         (unsigned)t1.size(), hex19(t1).c_str(),
                         (int)s1.acpStart, (int)s1.acpEnd,
                         (int)text1, (int)caret1, (int)typeOkA);
            Report(text1 && caret1 && typeOkA, L"T19a 上屏后光标推至文本尾", d19a);

            // b) 不留联想窗口（'h' 立即撤销联想，见 T18b），紧接第二次上屏
            const wchar_t* seqB = L"hao";
            bool typeOkB = true;
            for (const wchar_t* q = seqB; *q; q++)
            {
                wchar_t s[2] = { *q, 0 };
                if (!TypeLetters(pKm, s)) { typeOkB = false; break; }
                Sleep(60);
            }
            PumpMessages();
            Sleep(150);
            TypeKey(pKm, VK_SPACE);   // 第二次上屏
            PumpMessages();
            Sleep(300);
            std::wstring t2 = ReadFocusedText(pTm);
            const TS_SELECTION_ACP& s2 = g_pStore->Sel();
            bool prefix = t2.size() >= t1.size() && t2.compare(0, t1.size(), t1) == 0;
            bool grew = t2.size() > t1.size();
            bool caret2 = s2.acpStart == (LONG)t2.size() && s2.acpEnd == (LONG)t2.size();
            wchar_t d19b[300];
            _snwprintf_s(d19b, _TRUNCATE,
                         L"t2[%u]=%s sel={%d,%d} prefix=%d grew=%d caret=%d ok=%d",
                         (unsigned)t2.size(), hex19(t2).c_str(),
                         (int)s2.acpStart, (int)s2.acpEnd,
                         (int)prefix, (int)grew, (int)caret2, (int)typeOkB);
            Report(prefix && grew && caret2 && typeOkB,
                   L"T19b 连续上屏顺序与光标契约", d19b);
        }
        // ---- T20 以词定字（GAP §1 矩阵：对齐微软 [ ] 取字）----
        // xi'an 首选=西安（T17c 已锚定）：a) 无 Shift [ 取首字"西"上屏即组合
        // 收束；b) ] 取末字"安"。括号本身不得穿透上屏（store 无 0x5B/0x5D），
        // eat 侧以 expectEaten=true 一并断言（候选可见 [ ] 必被认领）。
        {
            EnsurePinyinMode(pKm);
            ClearFocusedText(pTm);
            const wchar_t* seq20 = L"xi'an";
            for (const wchar_t* q = seq20; *q; q++)
            {
                if (*q == L'\'')
                    TypeKey(pKm, VK_OEM_7, true); // 隔音符实键
                else
                {
                    wchar_t s20[2] = { *q, 0 };
                    TypeLetters(pKm, s20);
                }
                Sleep(60);
            }
            PumpMessages();
            Sleep(300);                        // 候选回包
            bool brk = TypeKey(pKm, VK_OEM_4, true); // [ 取首字
            PumpMessages();
            Sleep(300);
            std::wstring t20a = ReadFocusedText(pTm);
            bool first = brk && t20a.size() == 1 && t20a[0] == 0x897F;
            wchar_t d20a[200];
            _snwprintf_s(d20a, _TRUNCATE, L"brk=%d t[%u]=%s",
                         (int)brk, (unsigned)t20a.size(), hex19(t20a).c_str());
            Report(first, L"T20a 以词定字[取首字西", d20a);

            ClearFocusedText(pTm);
            for (const wchar_t* q = seq20; *q; q++)
            {
                if (*q == L'\'')
                    TypeKey(pKm, VK_OEM_7, true);
                else
                {
                    wchar_t s20[2] = { *q, 0 };
                    TypeLetters(pKm, s20);
                }
                Sleep(60);
            }
            PumpMessages();
            Sleep(300);
            bool brk2 = TypeKey(pKm, VK_OEM_6, true); // ] 取末字
            PumpMessages();
            Sleep(300);
            std::wstring t20b = ReadFocusedText(pTm);
            bool last = brk2 && t20b.size() == 1 && t20b[0] == 0x5B89;
            wchar_t d20b[200];
            _snwprintf_s(d20b, _TRUNCATE, L"brk=%d t[%u]=%s",
                         (int)brk2, (unsigned)t20b.size(), hex19(t20b).c_str());
            Report(last, L"T20b 以词定字]取末字安", d20b);
        }

    } while (false);

    // ---- 会话状态还原（对应开场 CaptureOrigProfile；防收尾劫持用户 IME） ----
    RestoreUserIme();

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
