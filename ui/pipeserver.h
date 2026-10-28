#pragma once
//
// LingxiUI.exe 侧管道服务端：
//  - 接受线程循环创建实例并等待连接（支持多宿主应用同时连接）；
//  - 每个客户端一个读线程；CTX → 入队异步预测 → 候选窗显示 + 回发候选列表；
//  - 候选窗跟随"正在打字的应用"（最后发 CTX 的客户端）；
//  - 预测工作线程：拼音每键即时预测、英文/续写 120ms 防抖 + LLM→n-gram 降级链。
//  - 客户端上下文以 shared_ptr 持有：断线不再裸 delete——后台重排线程可能
//    仍引用它（引用归零时析构并关闭管道句柄）。
//
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "window.h"
#include "engine/predictor.h"

namespace lingxi {

class PipeServer
{
public:
    bool Start(CandWindow* wnd, IPredictor* engine, IPredictor* pinyinEngine,
               ILlmReranker* reranker = nullptr);
    void Stop();

    void SendPick(uint32_t ver, uint32_t index); // 候选窗点击（UI 主线程）
    void SendInsertText(const std::wstring& text); // 符号面板：向当前活跃客户端插入文本

private:
    struct LearnRec // 最近一次拼音预测，用于选字反馈自学习
    {
        uint32_t ver = 0;
        std::wstring raw;              // 用户输入的原始拼音串
        std::vector<std::wstring> cands; // 全量候选
        std::vector<uint32_t> cover;   // 与 cands 平行：各候选消耗的 raw 前缀字符数（B1）
    };

    struct ClientCtx
    {
        ~ClientCtx()
        {
            // 管道句柄由最后一个引用者经此关闭（重排线程持引用期间绝不会被关）
            if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
        }
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        std::atomic<bool> alive{ true };
        std::atomic<uint32_t> gen{ 0 };   // 交互代：新预测/翻页/选词递增，重排据此放弃过期结果
        std::atomic<uint32_t> rrSeq{ 0 }; // 重排序号：新触发使旧线程立即退出（合并防堆积）
        bool active = false;
        bool dark = false;
        PipeServer* owner = nullptr;
        LearnRec lastLearn;
        std::vector<uint8_t> rxbuf; // 持久接收缓冲（跨 ReadFrame 保留多帧字节）
    };
    using ClientPtr = std::shared_ptr<ClientCtx>;

    struct PendingPredict
    {
        ClientPtr c;
        uint32_t ver = 0;
        std::wstring text;
        std::wstring ctx;    // 已上屏上文（V2 帧 CTX_F_CTX_PREFIX；LLM 重排用，可能为空）
        bool dark = false;
        bool pinyin = false; // true: text 是拼音字母串，走拼音引擎
        RECT caret{};
        uint64_t enqueuedAt = 0;
    };

    static DWORD WINAPI s_AcceptProc(LPVOID);
    void AcceptProc();
    static DWORD WINAPI s_ClientProc(LPVOID);
    void ClientProc(ClientPtr c);
    static DWORD WINAPI s_PredictProc(LPVOID);
    void PredictProc();
    void HandleMsg(ClientPtr c, uint8_t type, const std::vector<uint8_t>& body);
    void HandleCtx(ClientPtr c, const std::vector<uint8_t>& body);
    void EnqueuePredict(const ClientPtr& c, uint32_t ver, const std::wstring& text,
                        bool dark, const RECT& caret, bool pinyin,
                        const std::wstring& ctx = {});
    void SendCandidates(ClientCtx* c, uint32_t ver, const Prediction& pred);
    // 两阶段重排：原序已显示后，后台按上文重排 top-8，完成且未被交互打断则更新同 ver
    // cover 与 cands 平行（B1 部分提交），重排时按同一排列搬运
    void StartAsyncRerank(const ClientPtr& c, uint32_t ver, const std::vector<std::wstring>& cands,
                          const std::vector<uint32_t>& cover,
                          bool dark, const RECT& caret, const std::wstring& ctx,
                          uint32_t gen, uint32_t seq);
    void ApplyLearn(ClientCtx* c, uint32_t ver, uint32_t index); // 选字反馈→自学习
    void Activate(const ClientPtr& c);
    void Deactivate(const ClientPtr& c);
    // 判定本客户端是否仍是该 (ver,gen,seq) 重排的合法接收者（_cs 内）
    bool RerankStillCurrent(ClientCtx* c, uint32_t ver, uint32_t gen, uint32_t seq);

    CandWindow* _wnd = nullptr;
    IPredictor* _engine = nullptr;
    IPredictor* _pinyinEngine = nullptr; // M5 拼音引擎（可为 null：拼音模式退化为直拼）
    ILlmReranker* _reranker = nullptr;  // LLM 拼音候选重排（可为 null：保持原序）
    HANDLE _hThread = NULL;
    HANDLE _hPredictThread = NULL;
    HANDLE _hStop = NULL;
    HANDLE _hQueueSem = NULL;
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    ClientPtr _active;
    std::deque<PendingPredict> _queue;
};

} // namespace lingxi
