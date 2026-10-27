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
    bool Start(CandWindow* wnd, IPredictor* engine, IPredictor* pinyinEngine);
    void Stop();

    void SendPick(uint32_t ver, uint32_t index); // 候选窗点击（UI 主线程）
    void SendInsertText(const std::wstring& text); // 符号面板：向当前活跃客户端插入文本

private:
    struct LearnRec // 最近一次拼音预测，用于选字反馈自学习
    {
        uint32_t ver = 0;
        std::wstring raw;              // 用户输入的原始拼音串
        std::vector<std::wstring> cands; // 全量候选
    };

    struct ClientCtx
    {
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        std::atomic<bool> alive{ true };
        bool active = false;
        bool dark = false;
        PipeServer* owner = nullptr;
        LearnRec lastLearn;
        std::vector<uint8_t> rxbuf; // 持久接收缓冲（跨 ReadFrame 保留多帧字节）
    };

    struct PendingPredict
    {
        ClientCtx* c = nullptr;
        uint32_t ver = 0;
        std::wstring text;
        bool dark = false;
        bool pinyin = false; // true: text 是拼音字母串，走拼音引擎
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
                        bool dark, const RECT& caret, bool pinyin);
    void SendCandidates(ClientCtx* c, uint32_t ver, const Prediction& pred);
    void ApplyLearn(ClientCtx* c, uint32_t ver, uint32_t index); // 选字反馈→自学习
    void Activate(ClientCtx* c);
    void Deactivate(ClientCtx* c);

    CandWindow* _wnd = nullptr;
    IPredictor* _engine = nullptr;
    IPredictor* _pinyinEngine = nullptr; // M5 拼音引擎（可为 null：拼音模式退化为直拼）
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
