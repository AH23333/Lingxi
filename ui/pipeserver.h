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
#include <mutex>
#include <string>
#include <unordered_set>
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
        // H2：每客户端发送锁——PredictProc/重排线程/UI 线程三类并发 WriteFrame
        // 同一管道会字节级交错（PIPE_TYPE_BYTE），帧级串行化
        std::mutex sendMtx;
        std::atomic<uint32_t> gen{ 0 };   // 交互代：新预测/翻页/选词递增，重排据此放弃过期结果
        std::atomic<uint32_t> rrSeq{ 0 }; // 重排序号：新触发使旧线程立即退出（合并防堆积）
        std::atomic<uint32_t> sentSeq{ 0 }; // 整句触发序号（期二）：新触发使旧 worker 退出
        // 最近一帧已发候选快照（SendCandidates 统一记账，_cs 保护）：AI 整句追加在
        // "当前所示列表"之后——首帧/重排/追加三类发送都更新它，追加前按 ver 核对。
        uint32_t lastSentVer = 0;
        std::vector<std::wstring> lastSentCands;
        std::vector<uint32_t> lastSentCover;
        std::vector<std::wstring> lastSentAnno;
        bool active = false;
        bool dark = false;
        DWORD pid = 0;   // 宿主应用进程 PID（MSG_HELLO 上报；前台进程定位用）
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
        bool assoc = false;  // true: 联想帧（CTX_F_ASSOC）——LLM 长续写拆阶梯（GAP §11 多段呈现）
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
                        const std::wstring& ctx = {}, bool assoc = false);
    void SendCandidates(ClientCtx* c, uint32_t ver, const Prediction& pred);
    // 两阶段重排：原序已显示后，后台按上文重排 top-8，完成且未被交互打断则更新同 ver
    // cover（B1）/anno（GAP R6）各自与 cands 平行，重排时按同一排列搬运
    void StartAsyncRerank(const ClientPtr& c, uint32_t ver, const std::vector<std::wstring>& cands,
                          const std::vector<uint32_t>& cover,
                          const std::vector<std::wstring>& anno,
                          bool dark, const RECT& caret, const std::wstring& composing,
                          const std::wstring& ctx, uint32_t gen, uint32_t seq);
    // AI 整句（期二异步追加）：停顿窗 200ms + 三重陈旧闸 + 1.2s 独立预算生成，
    // 追加在"当前所示列表"末尾重发同 ver 候选帧。线程模型与重排一致（detached
    // worker + _rerankActive 在途计数 + SEH 外壳）。
    void StartAsyncSentence(const ClientPtr& c, uint32_t ver, const std::wstring& raw,
                            const std::wstring& pinyinSpaced,
                            bool dark, const RECT& caret, const std::wstring& ctx,
                            uint32_t gen, uint32_t seq);
    void ApplyLearn(ClientCtx* c, uint32_t ver, uint32_t index); // 选字反馈→自学习
    void Activate(const ClientPtr& c);
    void Deactivate(const ClientPtr& c);
    // 目标客户端选择：前台应用进程优先（_active 会被后台客户端激活/输入
    // 抢占——PredictProc 每次预测也改写它），无匹配回退 _active。
    // SendPick/SendInsertText 用。
    ClientPtr PickTargetClient();
    // 判定本客户端是否仍是该 (ver,gen,seq) 重排的合法接收者（_cs 内）
    bool RerankStillCurrent(ClientCtx* c, uint32_t ver, uint32_t gen, uint32_t seq);
    // 整句版陈旧判定（_cs 内）：ver/gen/sentSeq 三重 + alive/active
    bool SentenceCur(ClientCtx* c, uint32_t ver, uint32_t gen, uint32_t seq);

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
    std::vector<ClientPtr> _clients; // 在连客户端注册表（_cs 保护）：前台定位遍历用
    std::deque<PendingPredict> _queue;
    // H3：detached 线程在途计数（重排/整句/客户端读线程）——Stop 有界等归零
    // 再拆 _cs（线程尾部仍要进 _cs / 触引擎与窗口），防 delete 后 UAF
    std::atomic<int> _rerankActive{ 0 }; // 重排与整句 worker 共用（同一代 detached 模式）
    std::atomic<int> _clientActive{ 0 };
    // 整句触发用的音节表快照（_pinyinEngine->GetSyllableSet 惰性取一次；
    // 仅 PredictProc 单线程访问，无需锁）
    std::unordered_set<std::string> _syllSet;
    bool _syllInit = false;
    std::atomic<int> _sentDropStreak{ 0 }; // 整句连续丢弃计数（日志可观测丢弃频率）
    // 测试探针（非产品路径，deep_test T22 用）：%TEMP%\lingxi-probe-late-cands.flag
    // 存在时对 "probeprobe" 组合在 600ms 后以同 ver 重发迟到候选帧——注入 worker
    // 竞态。文件门而非 env 门：LingxiUI 会被任意宿主进程的 pipeclient 抢拉
    // （PITFALLS #66），env 不保证继承；旗标文件全进程可见，deep_test 开场建、
    // 收尾删。仅 Start 时查一次；生产常态无旗标恒 false。
    bool _probeLateCands = false;
};

} // namespace lingxi
