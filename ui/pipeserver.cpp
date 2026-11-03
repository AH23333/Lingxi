#include "pipeserver.h"

#include "../common/proto.h"
#include "../common/pipe_util.h"
#include "../common/settings.h"
#include "../common/log.h"
#include "usagestats.h"
#include "uitheme.h"
#include "autobackup.h"
#include "engine/calcnum.h"
#include "engine/uradical.h"

#include <atomic>
#include <thread>

namespace lingxi {

constexpr uint32_t kDebounceMs = 120;   // 预测防抖（仅英文/续写：拼音引擎 µs 级，无需防抖）
constexpr size_t kMaxQueue = 8;         // 有界队列
constexpr uint32_t kRerankSettleMs = 400;   // 停顿观察窗：连打中不算（用户继续敲=结果必作废，
                                            // 白烧 CPU），停下来"看一眼候选"时才算（体验实测
                                            // 120ms 时打字全程 0.7 核占用、重排几乎全作废）
constexpr uint32_t kRerankBudgetMs = 900;    // 两阶段重排总预算（不阻塞首次候选）

namespace {

// H3：detached 线程在途计数——创建方先加数（防 Stop 在线程起步前放行），
// 线程内 RAII 减数（覆盖全部退出路径）；Stop 有界等归零再拆 _cs
struct ActiveCount
{
    std::atomic<int>& c;
    ActiveCount(std::atomic<int>& n) : c(n) {}
    ~ActiveCount() { c.fetch_sub(1); }
};

// ---- SEH 外壳（专文 §7：llama 内部 AV 不能带崩 LingxiUI）----
// 规则：__try 所在函数不得有需析构的局部对象（MSVC C2712），全部经 POD
// 参数指针传递；C++ 临时对象只存在于被调函数帧。

struct RerankArgs
{
    ILlmReranker* r;
    const std::wstring* ctx;
    const std::vector<std::wstring>* cands;
    uint64_t deadline;
    std::vector<size_t>* out;
};
void RerankBody(void* a)
{
    auto* p = (RerankArgs*)a;
    *p->out = p->r->Rerank(*p->ctx, *p->cands, p->deadline);
}
bool RerankSeh(RerankArgs* a)
{
    __try { RerankBody(a); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct PredictArgs
{
    IPredictor* p;
    const std::wstring* text;
    Prediction* out;
};
void PredictBody(void* a)
{
    auto* p = (PredictArgs*)a;
    *p->out = p->p->Predict(*p->text);
}
bool PredictSeh(PredictArgs* a)
{
    __try { PredictBody(a); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

} // namespace

bool PipeServer::Start(CandWindow* wnd, IPredictor* engine, IPredictor* pinyinEngine,
                       ILlmReranker* reranker)
{
    _wnd = wnd;
    _engine = engine;
    _pinyinEngine = pinyinEngine;
    _reranker = reranker;
    InitializeCriticalSection(&_cs);
    _csInit = true;
    _hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    _hQueueSem = CreateSemaphoreW(NULL, 0, 64, NULL);

    _wnd->SetPickHandler([this](uint32_t ver, uint32_t idx) {
        // 鼠标点击：先转发给宿主上屏，再作为选字反馈喂给拼音引擎自学习
        ClientPtr a;
        EnterCriticalSection(&_cs);
        a = _active;
        LeaveCriticalSection(&_cs);
        SendPick(ver, idx);
        if (a) ApplyLearn(a.get(), ver, idx);
    });

    _hThread = CreateThread(NULL, 0, s_AcceptProc, this, 0, NULL);
    _hPredictThread = CreateThread(NULL, 0, s_PredictProc, this, 0, NULL);
    return _hThread != NULL && _hPredictThread != NULL;
}

void PipeServer::Stop()
{
    if (_hStop) SetEvent(_hStop);
    // 唤醒可能阻塞在信号量上的预测线程
    for (int i = 0; i < 16 && _hQueueSem; i++)
        ReleaseSemaphore(_hQueueSem, 1, NULL);
    bool predictDone = true, acceptDone = true;
    if (_hPredictThread)
    {
        predictDone = (WaitForSingleObject(_hPredictThread, 2000) == WAIT_OBJECT_0);
        CloseHandle(_hPredictThread);
        _hPredictThread = NULL;
    }
    if (_hThread)
    {
        acceptDone = (WaitForSingleObject(_hThread, 2000) == WAIT_OBJECT_0);
        CloseHandle(_hThread);
        _hThread = NULL;
    }
    // H3：有界等 detached 线程归零再拆 _cs——重排线程尾部 RerankStillCurrent、
    // 客户端读线程断连清理都要进 _cs，且会触 _wnd/_reranker（main 随后 delete）。
    // 重排预算 900ms+Settle 400ms，读线程 ReadFrame 见 _hStop 即醒，4s 充裕。
    // 必须先等预测线程退出再排空计数：它是重排线程的创建方，活着就可能在
    // 排空之后又 spawn 新重排线程（"等了一个线程、漏了另一个"）
    bool detachedDone = false;
    for (int i = 0; i < 400; i++)
    {
        detachedDone = (_rerankActive.load() <= 0 && _clientActive.load() <= 0);
        if (detachedDone) break;
        Sleep(10);
    }
    // H3：四类线程全部收敛才拆锁/拆信号量；任一超时（磁盘挂死、llama 卡死
    // 等病态场景）则保留 _cs 与 _hQueueSem 交给进程退出——僵尸线程仍会进
    // _cs / ReleaseSemaphore，先拆即 UB。_hStop 刻意不关：僵尸线程还要读。
    // H3 根治：main 域已改"停机不析构"（PipeServer/CandWindow/引擎堆上生、
    // 故意不 delete），超时僵尸线程触的本对象内存恒有效——保留锁不再是
    // "泄漏换安全"的权宜，而是完整收敛契约的一部分。
    if (predictDone && acceptDone && detachedDone)
    {
        if (_hQueueSem) { CloseHandle(_hQueueSem); _hQueueSem = NULL; }
        if (_csInit) { DeleteCriticalSection(&_cs); _csInit = false; }
    }
    else
    {
        LX_LOG(L"pipe", L"Stop：线程未收敛 predict=%d accept=%d detached=%d(rr=%d cl=%d)，"
               L"保留锁/信号量至进程退出",
               (int)predictDone, (int)acceptDone, (int)detachedDone,
               _rerankActive.load(), _clientActive.load());
    }
}

DWORD WINAPI PipeServer::s_AcceptProc(LPVOID p)
{
    ((PipeServer*)p)->AcceptProc();
    return 0;
}

void PipeServer::AcceptProc()
{
    bool first = true;
    for (;;)
    {
        if (WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT) break;

        HANDLE h = CreateLingxiPipeServerInstance(first);
        first = false;
        if (h == INVALID_HANDLE_VALUE)
        {
            // FILE_FLAG_FIRST_PIPE_INSTANCE 冲突 = 已有 LingxiUI 在运行，本实例退出
            if (GetLastError() == ERROR_ACCESS_DENIED)
            {
                LX_LOG(L"ui", L"检测到已有 LingxiUI 实例，退出当前进程");
                ExitProcess(0);
            }
            Sleep(500);
            continue;
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) { CloseHandle(h); break; }

        BOOL fConn = ConnectNamedPipe(h, &ov);
        if (!fConn && GetLastError() == ERROR_IO_PENDING)
        {
            HANDLE waits[2] = { _hStop, ov.hEvent };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1)
            {
                CancelIoEx(h, &ov);
                CloseHandle(ov.hEvent);
                CloseHandle(h);
                break;
            }
        }
        else if (!fConn && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(ov.hEvent);
            CloseHandle(h);
            continue;
        }
        CloseHandle(ov.hEvent);

        LX_LOG(L"ui", L"宿主客户端已接入 pipe=%p", h);
        ClientPtr c = std::make_shared<ClientCtx>();
        c->hPipe = h;
        c->owner = this;
        EnterCriticalSection(&_cs);
        _clients.push_back(c); // 客户端注册表：PickTargetClient 前台定位需遍历
        LeaveCriticalSection(&_cs);
        auto* param = new ClientPtr(c); // 客户端线程持一份引用
        _clientActive.fetch_add(1); // H3：先计数后 spawn——防 Stop 在线程起步前放行
        HANDLE t = CreateThread(NULL, 0, s_ClientProc, param, 0, NULL);
        if (!t)
        {
            _clientActive.fetch_sub(1); // H3：spawn 失败回退计数
            delete param; // 引用归零 → ClientCtx 析构并关闭句柄
            LX_LOG(L"ui", L"客户端线程创建失败");
        }
    }
}

DWORD WINAPI PipeServer::s_ClientProc(LPVOID p)
{
    auto* sp = (ClientPtr*)p;
    (*sp)->owner->ClientProc(*sp);
    delete sp;
    return 0;
}

void PipeServer::ClientProc(ClientPtr c)
{
    ActiveCount ac(_clientActive); // H3：Stop 等客户端读线程退出后再拆 _cs
    for (;;)
    {
        uint8_t type = 0;
        std::vector<uint8_t> body;
        bool rf = ReadFrame(c->hPipe, _hStop, c->rxbuf, type, body);
        if (!rf) break;
        HandleMsg(c, type, body);
    }

    // 标记死亡并摘除活动标记（预测工作线程据此丢弃结果）。
    // 注意：不在此处 CloseHandle——引用者（队列/重排线程）可能仍在使用；
    // 句柄关闭在 ClientCtx 析构（最后一个引用释放）时统一执行。
    c->alive = false;
    bool wasActive = false;
    EnterCriticalSection(&_cs);
    wasActive = (_active == c);
    c->active = false;
    if (wasActive) _active = nullptr;
    // 队列里属于本客户端的未决请求一并摘除（否则死客户端引用滞留到下次输入）
    for (auto it = _queue.begin(); it != _queue.end();)
    {
        if (it->c == c) it = _queue.erase(it);
        else ++it;
    }
    for (auto it = _clients.begin(); it != _clients.end();)
    {
        if (it->get() == c.get()) it = _clients.erase(it);
        else ++it;
    }
    LeaveCriticalSection(&_cs);
    if (wasActive) _wnd->Hide();

    LX_LOG(L"ui", L"宿主客户端断开");
}

void PipeServer::HandleMsg(ClientPtr c, uint8_t type, const std::vector<uint8_t>& body)
{
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();

    switch (type)
    {
    case MSG_HELLO:
    {
        MsgHello m{};
        if (ReadPod(p, end, m))
        {
            c->dark = m.darkTheme != 0;
            c->pid = m.pid; // 宿主应用 PID：PickTargetClient 按前台进程定位
        }
        break;
    }
    case MSG_ACTIVATE:
        Activate(c);
        break;
    case MSG_DEACTIVATE:
        Deactivate(c);
        break;
    case MSG_HIDE:
    {
        bool mine = false;
        EnterCriticalSection(&_cs);
        mine = (_active == c);
        LeaveCriticalSection(&_cs);
        if (mine) _wnd->Hide();
        break;
    }
    case MSG_CTX:
        HandleCtx(c, body);
        break;
    case MSG_PAGE:
    {
        MsgPage pg{};
        if (ReadPod(p, end, pg))
        {
            c->gen.fetch_add(1); // 翻页：进行中的重排结果过期
            _wnd->SetPage(pg.ver, pg.pageStart);
        }
        break;
    }
    case MSG_PICKED:
    {
        MsgPick pk{}; // 复用 ver+index 结构
        if (ReadPod(p, end, pk))
        {
            c->gen.fetch_add(1); // 已选词：进行中的重排结果过期
            ApplyLearn(c.get(), pk.ver, pk.index);
        }
        break;
    }
    case MSG_MODE_TOAST:
    {
        uint8_t english = 0;
        if (p < end) { memcpy(&english, p, 1); _wnd->ShowModeToast(english != 0); }
        break;
    }
    default:
        break;
    }
}

void PipeServer::Activate(const ClientPtr& c)
{
    EnterCriticalSection(&_cs);
    _active = c;
    c->active = true;
    LeaveCriticalSection(&_cs);
    LX_LOG(L"ui", L"客户端激活 pipe=%p", c->hPipe);
}

void PipeServer::Deactivate(const ClientPtr& c)
{
    bool wasActive = false;
    EnterCriticalSection(&_cs);
    wasActive = (_active == c);
    c->active = false;
    if (wasActive) _active = nullptr;
    LeaveCriticalSection(&_cs);
    if (wasActive) _wnd->Hide();
}

void PipeServer::HandleCtx(ClientPtr c, const std::vector<uint8_t>& body)
{
    // 双格式解析（按"POD 尺寸 + 字符串长度与帧总长自洽"判别）：
    //   V2 = 本版 MsgCtx（含 ctxChars）+ [可选 ctx 串] + text 串；
    //   V1 = 宿主进程里驻留的上一版 DLL 发的旧帧（MsgCtxV1，能改名不能覆盖，
    //        重建后老 DLL 会一直跑到宿主退出），必须兼容。
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();
    MsgCtx m{};
    std::wstring text;
    std::wstring ctx;

    bool parsed = false;
    if (body.size() >= sizeof(MsgCtx))
    {
        const uint8_t* q = p;
        MsgCtx m2{};
        if (ReadPod(q, end, m2) &&
            body.size() == sizeof(MsgCtx)
                              + (m2.ctxChars > 0 ? 2 + (size_t)m2.ctxChars * 2 : 0)
                              + 2 + (size_t)m2.textChars * 2) // 每串一个 u16 长度前缀
        {
            std::wstring ctxTmp, textTmp;
            if (m2.ctxChars > 0 && !ReadStr(q, end, ctxTmp)) return;
            if (!ReadStr(q, end, textTmp)) return;
            m = m2;
            ctx = std::move(ctxTmp);
            text = std::move(textTmp);
            parsed = true;
        }
    }
    if (!parsed)
    {
        const uint8_t* q = p;
        MsgCtxV1 m1{};
        if (ReadPod(q, end, m1) &&
            body.size() == sizeof(MsgCtxV1) + 2 + (size_t)m1.textChars * 2)
        {
            std::wstring textTmp;
            if (!ReadStr(q, end, textTmp)) return;
            m.ver = m1.ver;
            m.caretX = m1.caretX; m.caretY = m1.caretY;
            m.caretW = m1.caretW; m.caretH = m1.caretH;
            m.flags = m1.flags;
            m.textChars = m1.textChars;
            text = std::move(textTmp);
            parsed = true;
        }
    }
    if (!parsed) return; // 长度不自洽：损坏帧，丢弃

    LX_LOG(L"pipe", L"CTX ver=%u chars=%u ctxChars=%u flags=%u",
           m.ver, (uint32_t)text.size(), (uint32_t)ctx.size(), (uint32_t)m.flags);

    if (text.empty())
    {
        bool mine = false;
        EnterCriticalSection(&_cs);
        mine = (_active == c);
        LeaveCriticalSection(&_cs);
        if (mine) _wnd->Hide();
        return;
    }

    RECT rc = { m.caretX, m.caretY, m.caretX + m.caretW, m.caretY + m.caretH };
    EnqueuePredict(c, m.ver, text, c->dark, rc, (m.flags & CTX_F_PINYIN) != 0, ctx,
                   (m.flags & CTX_F_ASSOC) != 0);
}

void PipeServer::EnqueuePredict(const ClientPtr& c, uint32_t ver, const std::wstring& text,
                                 bool dark, const RECT& caret, bool pinyin,
                                 const std::wstring& ctx, bool assoc)
{
    PendingPredict e;
    e.c = c;
    e.ver = ver;
    e.text = text;
    e.ctx = ctx;
    e.dark = dark;
    e.caret = caret;
    e.pinyin = pinyin;
    e.assoc = assoc;
    e.enqueuedAt = GetTickCount64();

    EnterCriticalSection(&_cs);
    // 每客户端只保留最新一条（旧请求的处理结果会被 ver 检查丢弃，直接不入队）
    for (auto it = _queue.begin(); it != _queue.end();)
    {
        if (it->c == c) it = _queue.erase(it);
        else ++it;
    }
    if (_queue.size() >= kMaxQueue)
        _queue.pop_front(); // 全局有界，最旧的丢弃
    _queue.push_back(std::move(e));
    LeaveCriticalSection(&_cs);
    ReleaseSemaphore(_hQueueSem, 1, NULL);
}

DWORD WINAPI PipeServer::s_PredictProc(LPVOID p)
{
    ((PipeServer*)p)->PredictProc();
    return 0;
}

void PipeServer::PredictProc()
{
    for (;;)
    {
        WaitForSingleObject(_hQueueSem, INFINITE);
        if (WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT) break;

        PendingPredict e;
        bool have = false;
        // 取最新请求 + 防抖合并（打字是连续事件，等一等收后面的更新）。
        // 拼音请求不防抖：引擎 µs 级，"取最新"本身已合并——候选即时性优先
        // （120ms 防抖曾让快打+空格抢在候选回包前上屏字母原文）。
        for (;;)
        {
            {
                EnterCriticalSection(&_cs);
                if (!_queue.empty())
                {
                    e = _queue.back();
                    have = true;
                    // 只丢同客户端的更旧请求（"词框偶发不出现"诱因之二）：
                    // 多应用各挂 DLL（7+ 管道客户端），全局 clear() 会把其他
                    // 应用的帧静默吃掉——切换应用瞬间旧应用的尾巴帧挤掉新应用
                    // 的首帧。其余客户端的帧留在队列，由后续循环轮次处理
                    //（信号量每次 enqueue 已释放，空转轮自然消耗，无饥饿）。
                    for (auto it = _queue.begin(); it != _queue.end();)
                    {
                        if (it->c == e.c) it = _queue.erase(it);
                        else ++it;
                    }
                }
                LeaveCriticalSection(&_cs);
            }
            if (!have) break;
            if (e.pinyin) break; // 拼音：立刻处理
            uint64_t age = GetTickCount64() - e.enqueuedAt;
            if (age >= kDebounceMs) break;
            Sleep((DWORD)(kDebounceMs - age));
            // 睡醒后回到循环顶重新取：若期间有新请求则处理新的
        }
        if (!have) continue;
        if (WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT) break;

        // 预测期间不持任何锁（引擎可能耗时 300ms）
        // 拼音模式走专用引擎（组合字母串→汉字），否则走上下文降级链。
        // 降级链含 LLM（llama 第三方代码）：SEH 捕获硬件异常 → 本会话熔断，
        // 续写自然落到词表引擎；拼音引擎异常只丢当前帧（不熔断）。
        uint64_t tPred = GetTickCount64();
        Prediction pred;
        {
                    // 输入框"什么都没有"（纯空白上下文）不做续写/补全预测——用户报告空文本框
        // 弹预测面板遮挡屏幕；这类文本只配隐藏窗口。
        if (!e.pinyin)
        {
            bool blank = true;
            for (wchar_t ch : e.text)
                if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') { blank = false; break; }
            if (blank)
            {
                _wnd->Hide();
                continue;
            }
        }
        IPredictor* eng = (e.pinyin && _pinyinEngine) ? _pinyinEngine : _engine;
            PredictArgs pa{ eng, &e.text, &pred };
            if (!PredictSeh(&pa))
            {
                pred = Prediction{};
                if (eng == _engine && _reranker)
                {
                    _reranker->MarkBroken();
                    LX_LOG(L"pipe", L"预测引擎异常，LLM 已熔断（本会话词表兜底）");
                }
                else
                    LX_LOG(L"pipe", L"拼音引擎异常，本帧丢弃");
            }
        }
        if (WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT) break;
        LX_LOG(L"pipe", L"预测 ver=%u pinyin=%d 耗时=%llums -> %u 候选",
               e.ver, (int)e.pinyin, (unsigned long long)(GetTickCount64() - tPred),
               (uint32_t)pred.cands.size());

        if (!e.c || !e.c->alive) continue; // 客户端已断开，结果作废

        // 用户设置（每次请求实时读取，µs 级注册表查询）
        Settings st = LoadSettings();
        if (st.paused)
        {
            // 暂停预测：完全直通，不出候选（托盘“暂停预测”开关）
            _wnd->Hide();
            continue;
        }
        if (!e.pinyin && !st.enCompletion)
        {
            // 英文逐键补全关闭：纯英文上下文不出候选（中文上下文续写保留）
            bool asciiOnly = true;
            for (wchar_t ch : e.text)
                if (ch > 0x7F) { asciiOnly = false; break; }
            if (asciiOnly) pred.cands.clear();
        }
        // 拼音模式保留全量（候选窗分页、支持翻页）；英文补全/续写按设置页大小。
        // cover（B1）/anno（GAP R6）与 cands 必须同步裁剪/校验：不平行=旧引擎
        // 输出，按无 cover/无注音处理。
        if (pred.cover.size() != pred.cands.size()) pred.cover.clear();
        if (pred.anno.size() != pred.cands.size()) pred.anno.clear();
        size_t cap = e.pinyin ? (size_t)kMaxCandidates : (size_t)st.candidateCount;
        if (pred.cands.size() > cap)
        {
            pred.cands.resize(cap);
            if (!pred.cover.empty()) pred.cover.resize(cap);
            if (!pred.anno.empty()) pred.anno.resize(cap);
        }
        // 联想帧阶梯呈现（GAP §11 多段呈现）：LLM 长续写拆 [首段][全句] 两级，
        // 空格逐段连发/整句一步选（DLL 侧选词键+连发已有，仅缺候选塑形）。
        // 续写帧不拆——DedupCompletions 规则 ② 严禁前缀对，联想帧是唯一例外。
        if (e.assoc && !e.pinyin) StageAssocCands(pred);
        LX_LOG(L"pipe", L"裁剪后候选=%u paused=%d enComp=%d pinyin=%d",
               (uint32_t)pred.cands.size(), (int)st.paused, (int)st.enCompletion,
               (int)e.pinyin);

        {
            EnterCriticalSection(&_cs);
            _active = e.c;
            e.c->active = true;
            LeaveCriticalSection(&_cs);
        }
        // R7：明暗统一走 EffectiveDark（0 跟随系统=直读系统主题，旧"跟随应用"
        // 依赖 MsgHello.darkTheme 而 DLL 端恒发 0，跟随模式永远浅色，已退役）
        bool dark = EffectiveDark(st.darkMode);
        // B2：拼音请求把组合串传给候选窗首行回显（纯 UI 内部，不走协议）。
        // 0 候选拼音帧也显示（echo-only 窗）：v 临时英文（"v"/"vlog"）与无效
        // 拼音（"zhx"）没有候选但组合串需要回显，0 反馈=用户体感"按键失灵"。
        // 非拼音帧 0 候选维持隐藏（英文补全无候选不占屏）。
        if (!pred.cands.empty() || e.pinyin)
            _wnd->Show(e.caret, pred.cands, dark, e.ver, e.pinyin ? e.text : std::wstring(),
                       pred.anno);
        else
            _wnd->Hide();
        // 记录最近一次拼音预测供选字反馈自学习（临界区保护，跨线程读取）
        if (e.pinyin && !pred.cands.empty())
        {
            EnterCriticalSection(&_cs);
            e.c->lastLearn.ver = e.ver;
            e.c->lastLearn.raw = e.text;
            e.c->lastLearn.cands = pred.cands;
            e.c->lastLearn.cover = pred.cover;
            LeaveCriticalSection(&_cs);
        }
        SendCandidates(e.c.get(), e.ver, pred);

        // 两阶段重排：原序已显示，后台按上文重排 top-8（不阻塞首次候选）。
        // 门槛：≥2 候选（单候选重排是纯浪费）；ctx 非空（无上文时 LLM 无从判断
        // 连贯性，prompt 退化为频度复读，省一次调用）；陈旧闸 ≤400ms。
        // v 模式（数字大写/计算器）候选是确定性结果，重排纯浪费，跳过。
        // U 模式拆字（GAP §1）同理；双拼下 'u' 是键位、IsRadicalQuery 对键位
        // 串会误真，须全拼域判断（Predict/Learn 同门控）。
        if (e.pinyin && !IsCalcQuery(e.text) && (st.shuangpin || !IsRadicalQuery(e.text)) &&
            _reranker && st.llmRerank && !_reranker->Broken() &&
            pred.cands.size() >= 2 && !e.ctx.empty() &&
            GetTickCount64() - e.enqueuedAt <= 400)
        {
            uint32_t myGen = e.c->gen.fetch_add(1) + 1; // 本次预测占一个交互代
            uint32_t mySeq = e.c->rrSeq.fetch_add(1) + 1; // 使更早的重排线程作废（合并）
            StartAsyncRerank(e.c, e.ver, pred.cands, pred.cover, pred.anno, dark, e.caret,
                             e.text, e.ctx, myGen, mySeq); // B2：composing 穿透（pinyin 分支必真）
        }
    }
}

bool PipeServer::RerankStillCurrent(ClientCtx* c, uint32_t ver, uint32_t gen, uint32_t seq)
{
    EnterCriticalSection(&_cs);
    bool ok = c->alive && _active.get() == c && c->lastLearn.ver == ver &&
              c->gen.load() == gen && c->rrSeq.load() == seq;
    LeaveCriticalSection(&_cs);
    return ok;
}

void PipeServer::StartAsyncRerank(const ClientPtr& c, uint32_t ver,
                                  const std::vector<std::wstring>& cands,
                                  const std::vector<uint32_t>& cover,
                                  const std::vector<std::wstring>& anno,
                                  bool dark, const RECT& caret,
                                  const std::wstring& composing,
                                  const std::wstring& ctx, uint32_t gen, uint32_t seq)
{
    // 按值捕获全部上下文（线程独立，不依赖栈/队列）；c 为 shared_ptr 引用——
    // 客户端断线时 ClientCtx 不会被提前释放，本线程用完才归还（防悬垂）。
    _rerankActive.fetch_add(1); // H3：先计数后 spawn——防 Stop 在线程起步前放行
    std::thread([this, c, ver, cands, cover, anno, dark, caret, composing, ctx, gen, seq]() {
        ActiveCount ac(_rerankActive); // H3：Stop 有界等重排线程归零再拆 _cs
        // 合并连击：先观察一小段。若期间用户又敲了新键（新重排序号已占位），
        // 本线程立刻退出——拼音每键即时预测后，只有最后一键触发的重排有活干。
        Sleep(kRerankSettleMs);
        if (WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT) return;
        if (!c->alive || c->rrSeq.load() != seq) return;

        // 重排总预算 900ms（两阶段，不阻塞首次候选）；deadline 为绝对时刻
        uint64_t deadline = GetTickCount64() + kRerankBudgetMs;
        if (!RerankStillCurrent(c.get(), ver, gen, seq)) return;

        std::vector<size_t> order;
        RerankArgs ra{ _reranker, &ctx, &cands, deadline, &order };
        if (!RerankSeh(&ra))
        {
            _reranker->MarkBroken(); // llama 内部 AV：本会话熔断，不再重试
            LX_LOG(L"pipe", L"重排引擎异常，本次回退原序（LLM 已熔断）");
            return;
        }
        if (order.empty()) return;

        // 核对：客户端仍在、仍活跃、ver 未被新预测覆盖、期间无翻页/选词/新重排
        if (!RerankStillCurrent(c.get(), ver, gen, seq)) return;

        // 重组：重排 top-K + 其余候选保持原序；cover（B1）/anno（GAP R6）
        // 按同一排列搬运，不平行则视为无 cover/无注音（不影响正确性）。
        size_t k = min(cands.size(), order.size());
        bool hasCover = cover.size() == cands.size();
        bool hasAnno = anno.size() == cands.size();
        std::vector<std::wstring> reordered;
        std::vector<uint32_t> reorderedCover;
        std::vector<std::wstring> reorderedAnno;
        reordered.reserve(cands.size());
        reorderedCover.reserve(hasCover ? cands.size() : 0);
        reorderedAnno.reserve(hasAnno ? cands.size() : 0);
        for (size_t idx : order)
        {
            reordered.push_back(cands[idx]);
            if (hasCover) reorderedCover.push_back(cover[idx]);
            if (hasAnno) reorderedAnno.push_back(anno[idx]);
        }
        for (size_t i = k; i < cands.size(); i++)
        {
            reordered.push_back(cands[i]);
            if (hasCover) reorderedCover.push_back(cover[i]);
            if (hasAnno) reorderedAnno.push_back(anno[i]);
        }

        Prediction pred2;
        pred2.cands = reordered;
        pred2.replaceLen = 0;
        pred2.cover = std::move(reorderedCover);
        pred2.anno = reorderedAnno; // 拷贝：下方 Show 还要用 reorderedAnno
        // 回到第一页并更新候选窗（同 ver，宿主无需感知；composing 同 ver 不跳变）。
        // H4：经 PostShow 封包投递 UI 线程处理——detached 线程不再直调 Show
        //（与主线程鼠标/绘制竞态窗口大；ver 不符的陈旧封包在 UI 线程丢弃）
        _wnd->PostShow(caret, reordered, dark, ver, composing, reorderedAnno);
        SendCandidates(c.get(), ver, pred2);
        LX_LOG(L"pipe", L"LLM 重排生效(两阶段) ver=%u k=%u", ver, (uint32_t)k);
    }).detach();
}

void PipeServer::SendCandidates(ClientCtx* c, uint32_t ver, const Prediction& pred)
{
    std::vector<uint8_t> bodyOut;
    MsgCands m{};
    m.ver = ver;
    m.count = (uint32_t)pred.cands.size();
    m.replaceLen = pred.replaceLen;
    AppendPod(bodyOut, m);
    for (const auto& s : pred.cands)
        AppendStr(bodyOut, s);
    // B1 部分提交：cover 与 cands 平行且有效时，在串列表之后追加 u16×count。
    // 旧 DLL 顺序读完字符串即停（容忍尾部）；新 DLL 读不到数组则按全 0 处理。
    if (pred.cover.size() == pred.cands.size() && !pred.cover.empty())
    {
        for (uint32_t cv : pred.cover)
        {
            uint16_t v = (uint16_t)min(cv, 0xFFFFu);
            AppendPod(bodyOut, v);
        }
    }
    // H2：整帧持锁写——与重排线程/选词/符号插入的并发写帧串行化
    std::lock_guard<std::mutex> lk(c->sendMtx);
    WriteFrame(c->hPipe, MSG_CANDS, bodyOut.data(), (uint32_t)bodyOut.size(), 500);
}

// 目标客户端选择：候选窗点选/面板插入必须落进前台应用。_active 会被后台
// 客户端抢占——Activate 置位、PredictProc 每次预测也改写；宿主重启后的
// 重连风暴里最后激活者未必是前台应用（实锤：插入发往后台实例 → 该实例
// GetFocus NULL → 文本静默丢失）。规则：优先取 MSG_HELLO 上报 PID 与前台
// 窗口进程一致的客户端；前台仍是本宿主进程时（面板 ExitSearch 刚发出
// SetForegroundWindow、激活在途）短等迁移；无匹配回退 _active。
PipeServer::ClientPtr PipeServer::PickTargetClient()
{
    const DWORD myPid = GetCurrentProcessId();
    DWORD fgPid = 0;
    for (int i = 0;; i++)
    {
        HWND fg = GetForegroundWindow();
        fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid != myPid || i >= 25) break; // 前台已离开宿主，或已等 ~500ms
        Sleep(20);
    }
    bool viaFg = false;
    ClientPtr pick;
    if (fgPid && fgPid != myPid)
    {
        EnterCriticalSection(&_cs);
        if (_active && _active->alive && _active->pid == fgPid) pick = _active;
        if (!pick)
            for (auto& c : _clients)
                if (c->alive && c->pid == fgPid) { pick = c; break; }
        LeaveCriticalSection(&_cs);
        viaFg = pick != nullptr;
    }
    if (!pick)
    {
        EnterCriticalSection(&_cs);
        if (_active && _active->alive) pick = _active;
        LeaveCriticalSection(&_cs);
    }
    LX_LOG(L"ui", L"PickTarget fgPid=%u -> pid=%u %s", fgPid,
           pick ? pick->pid : 0,
           viaFg ? L"前台命中" : (pick ? L"回退_active" : L"无客户端"));
    return pick;
}

void PipeServer::SendPick(uint32_t ver, uint32_t index)
{
    ClientPtr a = PickTargetClient();
    if (!a || !a->alive) return;

    MsgPick m{ ver, index };
    {
        std::lock_guard<std::mutex> lk(a->sendMtx); // H2：与预测/重排回包串行化
        WriteFrame(a->hPipe, MSG_PICK, &m, sizeof(m), 200);
    }
    LX_LOG(L"ui", L"PICK ver=%u index=%u", ver, index);
}

// 符号面板：把符号文本发给目标客户端（前台应用优先），DLL 侧经封送窗口插入。
void PipeServer::SendInsertText(const std::wstring& text)
{
    ClientPtr a = PickTargetClient();
    if (!a || !a->alive)
    {
        LX_LOG(L"ui", L"SendInsertText 丢弃：无可用客户端");
        return;
    }
    std::vector<uint8_t> body;
    AppendStr(body, text);
    {
        std::lock_guard<std::mutex> lk(a->sendMtx); // H2：与预测/重排回包串行化
        WriteFrame(a->hPipe, MSG_INSERT_TEXT, body.data(), (DWORD)body.size(), 200);
    }
    LX_LOG(L"ui", L"SendInsertText 已发 chars=%u pid=%u", (uint32_t)text.size(), a->pid);
}

// 选字反馈（键盘/鼠标）：按 ver 取最近一次拼音预测，用全局索引定位被选词，
// 把（原始拼音串, 词）喂给拼音引擎做自学习。
// B1：学习键改用候选实际消耗的前缀（cover）——旧实现把整条 raw 记给部分词
// （"nihao"选"你"记到 nihao→你），查询键永不相交=错位学习；现在记 ni→你，
// 后续打 ni 即可命中。cover 缺失/为 0 时维持旧口径（整条 raw）。
void PipeServer::ApplyLearn(ClientCtx* c, uint32_t ver, uint32_t index)
{
    // 无痕模式（批次 G）：选词不留痕——不学习/不计统计/不触发每日备份与
    // 目标提醒。单键读注册表（µs 级）保精确：托盘开关下一选词即生效。
    // 预测本身不受影响（查询走既有词典，无痕只停"写"侧）。
    if (IncognitoActive()) return;
    std::wstring raw, word;
    uint32_t cover = 0;
    EnterCriticalSection(&_cs);
    if (c && c->lastLearn.ver == ver && index < c->lastLearn.cands.size())
    {
        raw = c->lastLearn.raw;
        word = c->lastLearn.cands[index];
        cover = index < c->lastLearn.cover.size() ? c->lastLearn.cover[index] : 0;
    }
    LeaveCriticalSection(&_cs);
    StatsAddPick(word.c_str()); // 打字统计（空词/沙箱宿主内部自动跳过）
    AutoBackupTick();           // 每日数据备份（当日已备/沙箱内部短路，µs 级）
    // 每日字数目标达成提醒（R8 统计增强）：达成当日仅一次候选窗气泡；
    // 目标 0=关闭；沙箱宿主统计全零天然不触发。
    // goal 读取每 16 次选词才刷新一次缓存：LoadSettings 是全量注册表读
    // （约 30 项），每次选词都读纯属热路径浪费；原子量防多客户端线程
    // 竞态，读到新旧值均无害（达成检测本身幂等）。
    static std::atomic<uint32_t> s_goalChecks{0};
    static std::atomic<uint32_t> s_goalCache{0};
    if ((++s_goalChecks & 15) == 1)
        s_goalCache.store(LoadSettings().dailyCharGoal);
    if (_wnd && !word.empty())
    {
        uint32_t goal = s_goalCache.load();
        if (goal > 0)
        {
            UsageData ud = StatsSnapshot();
            if (ud.dayChars >= goal && ud.goalReachedDay != ud.dayKey)
            {
                StatsMarkGoalReached(ud.dayKey);
                _wnd->ShowTextToast(L"今日目标达成");
                LX_LOG(L"pipe", L"每日目标达成提醒 day=%u chars=%llu goal=%u",
                       (unsigned)ud.dayKey, (unsigned long long)ud.dayChars,
                       (unsigned)goal);
            }
        }
    }
    if (cover > 0 && cover < raw.size())
        raw = raw.substr(0, cover); // 学习键=消耗前缀
    if (!raw.empty() && !word.empty() && _pinyinEngine)
    {
        _pinyinEngine->Learn(raw, word);
        LX_LOG(L"pipe", L"自学习反馈 ver=%u idx=%u cover=%u '%ls'",
               ver, index, cover, word.c_str());
    }
}

} // namespace lingxi
