#pragma once
//
// ImeCore.dll 侧的管道客户端：
//  - Activate 时启动后台线程；若 LingxiUI.exe 未运行则自动拉起（失败不致命）；
//  - 循环重连；断开后回到重连循环，直到 Stop；
//  - 读线程只做两件事：更新候选（经回调+临界区）、把鼠标选词 PostMessage 到封送窗口；
//  - 发送在调用线程完成（overlapped，80ms 超时，超时丢弃本条消息）。
//
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

extern HINSTANCE g_hInst; // 全局定义于 dllmain.cpp
extern LONG g_cThreadRefs; // L4：DLL 卸载判定的线程在途引用计数，定义于 dllmain.cpp

namespace lingxi {

class IPipeClientCallback
{
public:
    // cover：与 cands 平行的"各候选消耗的原始拼音前缀字符数"（B1 部分提交）；
    // 旧 UI 不发送时为全 0（调用方按全清处理）。长度恒等于 cands.size()。
    virtual void OnCandidates(uint32_t ver, uint32_t replaceLen,
                              std::vector<std::wstring>&& cands,
                              std::vector<uint32_t>&& cover) = 0;
    virtual void OnPickRequested(uint32_t ver, uint32_t index) = 0;
protected:
    ~IPipeClientCallback() = default;
};

class CPipeClient
{
public:
    CPipeClient() = default;
    ~CPipeClient() { Stop(); }

    void Start(IPipeClientCallback* cb, HWND hwndMarshal);
    void Stop();

    bool Send(uint8_t type, const void* body, uint32_t bodyLen, uint32_t timeoutMs = 80);
    bool SendStr(uint8_t type, const void* prefixBody, uint32_t prefixLen,
                 const std::wstring& str, uint32_t timeoutMs = 80);
    bool IsConnected();

private:
    static DWORD WINAPI s_ThreadProc(LPVOID);
    void Run();
    void HandleMessage(uint8_t type, const std::vector<uint8_t>& body);
    bool EnsureUIRunning(); // 拉起 LingxiUI.exe（与 DLL 同目录），已运行则跳过

    IPipeClientCallback* _cb = nullptr;
    HWND _hwndMarshal = nullptr;
    HANDLE _hThread = NULL;
    HANDLE _hStop = NULL;
    HANDLE _hPipe = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    bool _connected = false;
    uint64_t _lastFailTick = 0; // 发送失败后的退避窗口（500ms 内直接丢弃）
};

} // namespace lingxi
