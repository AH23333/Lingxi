#pragma once
//
// LingxiUI.exe 侧管道服务端：
//  - 接受线程循环创建实例并等待连接（支持多宿主应用同时连接）；
//  - 每个客户端一个读线程；CTX → 入队异步预测 → 候选窗显示 + 回发候选列表；
//  - 候选窗跟随"正在打字的应用"（最后发 CTX 的客户端）；
//  - 预测工作线程：120ms 防抖合并 + LLM→n-gram 降级链 + 客户端存活检查。
//
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "window.h"
#include "engine/predictor.h"

namespace lingxi {

class PipeServer
{
public:
    bool Start(CandWindow* wnd, IPredictor* engine);
    void Stop();

    void SendPick(uint32_t ver, uint32_t index); // 候选窗点击（UI 主线程）

private:
    struct ClientCtx
    {
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        std::atomic<bool> alive{ true };
        bool active = false;
        bool dark = false;
        PipeServer* owner = nullptr;
    };

    struct PendingPredict
    {
        ClientCtx* c = nullptr;
        uint32_t ver = 0;
        std::wstring text;
        bool dark = false;
        RECT caret{};
        uint64_t enqueuedAt = 0;
    };

    static DWORD WINAPI s_AcceptProc(LPVOID);
    void AcceptProc();
    static DWORD WINAPI s_ClientProc(LPVOID);
    void ClientProc(ClientCtx* c);
    static DWORD WINAPI s_PredictProc(LPVOID);
    void PredictProc();
    void HandleMsg(ClientCtx* c, uint8_t type, const std::vector<uint8_t>& body);
    void HandleCtx(ClientCtx* c, const std::vector<uint8_t>& body);
    void EnqueuePredict(ClientCtx* c, uint32_t ver, const std::wstring& text,
                        bool dark, const RECT& caret);
    void SendCandidates(ClientCtx* c, uint32_t ver, const Prediction& pred);
    void Activate(ClientCtx* c);
    void Deactivate(ClientCtx* c);

    CandWindow* _wnd = nullptr;
    IPredictor* _engine = nullptr;
    HANDLE _hThread = NULL;
    HANDLE _hPredictThread = NULL;
    HANDLE _hStop = NULL;
    HANDLE _hQueueSem = NULL;
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    ClientCtx* _active = nullptr;
    std::deque<PendingPredict> _queue;
};

} // namespace lingxi
