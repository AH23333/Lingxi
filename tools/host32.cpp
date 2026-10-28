//
// host32.cpp —— 32 位 TIP 验证宿主（R5 32 位 TIP 真机取证，v2）。
// deep_test 同款文本宿主骨架（窗口 + ThreadMgr + TestStore + AssociateFocus），
// 激活灵犀 zh-CN profile 后 32 位 msctf 经 WOW6432Node CLSID 在本进程内
// 加载 bin\Debug32\ImeCore.dll；模块枚举证明加载路径与位数；管道连接证据
// 由 %TEMP%\lingxi-ime.log 增量归因（[host32] 标记行 + [pipe] 行同文件）。
// 退出码：0=32 位 ImeCore 已载；4=未找到。
//
#include <initguid.h>
#include <windows.h>
#include <msctf.h>
#include <textstor.h>
#include <tlhelp32.h>
#include <string>
#include <cstdio>
#include "../common/guids.h"
#include "../common/log.h"

// ==================== 合成文本存储（deep_test 同款） ====================

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
        if (_sink) _sink->OnTextChange(0, 0);
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
        if (_sink) _sink->OnTextChange(0, 0);
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
    LONG _cRef;
    std::wstring _buf;
    TS_SELECTION_ACP _sel;
    ITextStoreACPSink* _sink = nullptr;
};

static void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static bool FindImeCoreModule(std::wstring& pathOut)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me))
    {
        do {
            if (_wcsicmp(me.szModule, L"ImeCore.dll") == 0)
            {
                pathOut = me.szExePath;
                found = true;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

int wmain()
{
    lingxi::InitLogging();
    LX_LOG(L"host32", L"32 位宿主启动 pid=%u", GetCurrentProcessId());
    wprintf(L"host32 pid=%u\n", GetCurrentProcessId());

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed hr=%08lx\n", hr); return 2; }

    // ---- 宿主窗口（TIP 实例化需要焦点上下文）----
    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LingxiHost32Wnd";
    RegisterClassW(&wc);
    HWND hwndHost = CreateWindowW(L"LingxiHost32Wnd", L"Lingxi Host32 Verify", WS_OVERLAPPEDWINDOW,
                                  40, 40, 520, 200, NULL, NULL, hInst, NULL);
    if (!hwndHost) { wprintf(L"CreateWindowW failed\n"); return 3; }
    ShowWindow(hwndHost, SW_SHOW);
    SetForegroundWindow(hwndHost);
    SetFocus(hwndHost);
    PumpMessages();

    // ---- TSF 宿主 ----
    ITfThreadMgr* pTm = nullptr;
    hr = CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void**)&pTm);
    if (FAILED(hr) || !pTm) { wprintf(L"ThreadMgr create failed\n"); return 3; }
    TfClientId cid = 0;
    hr = pTm->Activate(&cid);
    wprintf(L"ThreadMgr::Activate hr=%08lx\n", hr);

    TestStore* pStore = new TestStore();
    ITfDocumentMgr* pDoc = nullptr;
    hr = pTm->CreateDocumentMgr(&pDoc);
    TfEditCookie ecStore = 0;
    ITfContext* pCtx = nullptr;
    hr = pDoc->CreateContext(cid, 0, pStore, &pCtx, &ecStore);
    wprintf(L"CreateContext hr=%08lx\n", hr);
    if (pCtx) { pDoc->Push(pCtx); pCtx->Release(); }
    ITfDocumentMgr* pPrev = nullptr;
    hr = pTm->AssociateFocus(hwndHost, pDoc, &pPrev);
    wprintf(L"AssociateFocus hr=%08lx\n", hr);
    if (pPrev) pPrev->Release();
    SetFocus(hwndHost);
    PumpMessages();

    // ---- 激活灵犀 zh-CN（ProfileMgr 完整路径，PITFALLS #32）----
    ITfInputProcessorProfileMgr* pPm = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, (void**)&pPm);
    if (FAILED(hr) || !pPm) { wprintf(L"ProfileMgr create failed\n"); return 3; }
    hr = pPm->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804,
                              CLSID_LingxiTextService, GUID_LingxiProfile_zhCN,
                              NULL, 0);
    wprintf(L"ActivateProfile hr=%08lx\n", hr);
    LX_LOG(L"host32", L"ActivateProfile hr=%08lx", hr);

    // ---- 等待 32 位 DLL 加载（最多 8s）----
    std::wstring modPath;
    bool found = false;
    for (int i = 0; i < 32 && !found; i++)
    {
        PumpMessages();
        Sleep(250);
        found = FindImeCoreModule(modPath);
    }
    bool is32Dir = found && modPath.find(L"32") != std::wstring::npos;
    if (found)
    {
        wprintf(L"loaded ImeCore: %ls (dir32=%d)\n", modPath.c_str(), (int)is32Dir);
        LX_LOG(L"host32", L"loaded ImeCore: %ls (dir32=%d)", modPath.c_str(), (int)is32Dir);
    }
    // 再泵 2s 让管道连接行落盘
    for (int i = 0; i < 8; i++) { PumpMessages(); Sleep(250); }

    LX_LOG(L"host32", L"32 位宿主退出 found=%d dir32=%d", (int)found, (int)is32Dir);
    if (pPm) pPm->Release();
    if (pDoc) pDoc->Release();
    if (pTm) pTm->Deactivate();
    PumpMessages();
    CoUninitialize();
    return (found && is32Dir) ? 0 : 4;
}
