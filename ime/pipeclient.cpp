#include "pipeclient.h"

#include "../common/proto.h"
#include "../common/pipe_util.h"
#include "../common/log.h"

namespace lingxi {

void CPipeClient::Start(IPipeClientCallback* cb, HWND hwndMarshal)
{
    _cb = cb;
    _hwndMarshal = hwndMarshal;
    if (!_csInit) { InitializeCriticalSection(&_cs); _csInit = true; }
    _hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    // L4：创建线程前先持引用——CreateThread 返回到线程函数真正跑起来之间
    // 若 COM 恰好查询卸载并 FreeLibrary，未启动的线程入口同样失效。
    // 创建失败则立即归还（无线程来减这一笔）。
    InterlockedIncrement(&g_cThreadRefs);
    _hThread = CreateThread(NULL, 0, s_ThreadProc, this, 0, NULL);
    if (!_hThread) InterlockedDecrement(&g_cThreadRefs);
}

void CPipeClient::Stop()
{
    if (_hStop) SetEvent(_hStop);
    if (_hThread)
    {
        bool exited = WaitForSingleObject(_hThread, 3000) == WAIT_OBJECT_0;
        CloseHandle(_hThread);
        _hThread = NULL;
        // M3：超时=线程仍存活且仍在用 _cs/_cb——此时拆临界区/清回调=UAF
        // 崩宿主。不拆不删，宁泄漏（进程卸载路径 3s 内正常必收敛，走到
        // 这里说明宿主已卡死，泄漏无实质影响）。
        if (!exited)
        {
            LX_LOG(L"pipe", L"Stop：工作线程 3s 未退，保留 _cs/_cb 防 UAF");
            return;
        }
    }
    if (_hStop) { CloseHandle(_hStop); _hStop = NULL; }
    if (_csInit) { DeleteCriticalSection(&_cs); _csInit = false; }
    _cb = nullptr;
}

DWORD WINAPI CPipeClient::s_ThreadProc(LPVOID p)
{
    ((CPipeClient*)p)->Run();
    InterlockedDecrement(&g_cThreadRefs); // L4：Run 退出=线程彻底离开 DLL 代码，归还 Start 持有的引用
    return 0;
}

bool CPipeClient::EnsureUIRunning()
{
    // UI 进程用 FILE_FLAG_FIRST_PIPE_INSTANCE 自我互斥：重复拉起无害。
    wchar_t dllPath[MAX_PATH] = L"";
    if (GetModuleFileNameW(g_hInst, dllPath, MAX_PATH) == 0) return false;
    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    wchar_t exePath[MAX_PATH];
    _snwprintf_s(exePath, _TRUNCATE, L"%sLingxiUI.exe", dllPath);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LX_LOG(L"pipe", L"已拉起 LingxiUI.exe");
        return true;
    }

    // 32 位 DLL（bin\<C>32\）目录里没有 x64 LingxiUI.exe：回退到姊妹 x64 目录
    //（按 DLL 自身配置名对齐 Debug/Release——32 位进程拉起 x64 exe 合法）。
    // 仍失败则降级纯直通（现有安全路径不变）。
    const wchar_t* sibling = wcsstr(dllPath, L"Debug32") ? L"Debug" : L"Release";
    _snwprintf_s(exePath, _TRUNCATE, L"%s..\\%s\\LingxiUI.exe", dllPath, sibling);
    if (CreateProcessW(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LX_LOG(L"pipe", L"已拉起 LingxiUI.exe（32 位回退路径）");
        return true;
    }
    LX_LOG(L"pipe", L"LingxiUI.exe 启动失败 err=%u（预测功能降级为纯直通）", GetLastError());
    return false;
}

void CPipeClient::Run()
{
    std::vector<uint8_t> rxbuf; // 持久接收缓冲（跨 ReadFrame 保留多帧字节）
    for (;;)
    {
        // ---- 连接阶段 ----
        bool firstTry = true;
        bool launched = false;
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        while (WaitForSingleObject(_hStop, 0) == WAIT_TIMEOUT)
        {
            hPipe = OpenLingxiPipeClient();
            if (hPipe != INVALID_HANDLE_VALUE) break;
            if (firstTry)
            {
                firstTry = false;
                launched = EnsureUIRunning(); // 仅首次尝试拉起
            }
            if (WaitForSingleObject(_hStop, launched ? 1000 : 300) != WAIT_TIMEOUT) break;
        }
        if (hPipe == INVALID_HANDLE_VALUE || WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT)
        {
            if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
            break;
        }

        {
            EnterCriticalSection(&_cs);
            _hPipe = hPipe;
            _connected = true;
            LeaveCriticalSection(&_cs);
        }
        LX_LOG(L"pipe", L"管道已连接");

        // 连接后先发 HELLO（协议版本、宿主 PID）+ ACTIVATE 快照。
        // ACTIVATE 必须在连接后补发：ActivateEx 里发的那条发生在连接前，会丢。
        {
            MsgHello hello{ kProtoVer, GetCurrentProcessId(), 0 };
            WriteFrame(hPipe, MSG_HELLO, &hello, sizeof(hello), 200);
            WriteFrame(hPipe, MSG_ACTIVATE, NULL, 0, 200);
        }

        // ---- 读写阶段 ----
        rxbuf.clear();
        for (;;)
        {
            uint8_t type = 0;
            std::vector<uint8_t> body;
            if (!ReadFrame(hPipe, _hStop, rxbuf, type, body)) break;
            HandleMessage(type, body);
        }

        {
            EnterCriticalSection(&_cs);
            _connected = false;
            _hPipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&_cs);
        }
        CloseHandle(hPipe);
        LX_LOG(L"pipe", L"管道断开，准备重连");

        if (WaitForSingleObject(_hStop, 1000) != WAIT_TIMEOUT) break;
    }
}

void CPipeClient::HandleMessage(uint8_t type, const std::vector<uint8_t>& body)
{
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();

    switch (type)
    {
    case MSG_CANDS:
    {
        MsgCands m{};
        if (!ReadPod(p, end, m)) return;
        std::vector<std::wstring> cands;
        cands.reserve(min(m.count, 16u));
        for (uint32_t i = 0; i < m.count; i++)
        {
            std::wstring s;
            if (!ReadStr(p, end, s)) return;
            cands.push_back(std::move(s));
        }
        // B1 部分提交：串列表之后的 u16 cover 数组（可选尾部）。
        // 新 UI 追加发送；旧 UI 没有 → 读失败按全 0（提交后全清=旧行为）。
        std::vector<uint32_t> cover(m.count, 0);
        for (uint32_t i = 0; i < m.count; i++)
        {
            uint16_t cv = 0;
            if (!ReadPod(p, end, cv)) break; // 帧尾：旧 UI，保持已填 0
            cover[i] = cv;
        }
        if (_cb) _cb->OnCandidates(m.ver, m.replaceLen, std::move(cands), std::move(cover));
        break;
    }
    case MSG_PICK:
    {
        MsgPick m{};
        if (!ReadPod(p, end, m)) return;
        if (_hwndMarshal)
            PostMessageW(_hwndMarshal, WM_APP_LX_PICK, m.index, m.ver);
        break;
    }
    case MSG_INSERT_TEXT:
    {
        std::wstring* s = new std::wstring();
        if (!ReadStr(p, end, *s)) { delete s; return; }
        LX_LOG(L"pipe", L"MSG_INSERT_TEXT 收到 chars=%u marshal=%p", (uint32_t)s->size(), (void*)_hwndMarshal);
        if (_hwndMarshal)
            PostMessageW(_hwndMarshal, WM_APP_LX_INSERT, 0, (LPARAM)s);
        else
            delete s;
        break;
    }
    default:
        break;
    }
}

bool CPipeClient::Send(uint8_t type, const void* body, uint32_t bodyLen, uint32_t timeoutMs)
{
    // 未 Start（激活失败路径可能先发 DEACTIVATE）：临界区尚未初始化，直接拒绝
    if (!_csInit) return false;
    // 写操作整体串行化：HELLO/ACTIVATE（管道线程）与 SendCtx（宿主 UI 线程）
    // 并发写会交错出损坏帧。临界区内完成整帧写入（最坏 80ms）。
    EnterCriticalSection(&_cs);
    bool ok = false;
    if (_connected && _hPipe != INVALID_HANDLE_VALUE &&
        GetTickCount64() - _lastFailTick >= 500)
    {
        ok = WriteFrame(_hPipe, type, body, bodyLen, timeoutMs);
        if (!ok) _lastFailTick = GetTickCount64();
    }
    LeaveCriticalSection(&_cs);
    return ok;
}

bool CPipeClient::SendStr(uint8_t type, const void* prefixBody, uint32_t prefixLen,
                          const std::wstring& str, uint32_t timeoutMs)
{
    std::vector<uint8_t> body;
    if (prefixLen)
    {
        const uint8_t* p = (const uint8_t*)prefixBody;
        body.insert(body.end(), p, p + prefixLen);
    }
    AppendStr(body, str);
    return Send(type, body.data(), (uint32_t)body.size(), timeoutMs);
}

bool CPipeClient::IsConnected()
{
    EnterCriticalSection(&_cs);
    bool c = _connected;
    LeaveCriticalSection(&_cs);
    return c;
}

} // namespace lingxi
