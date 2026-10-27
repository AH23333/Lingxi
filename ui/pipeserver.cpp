#include "pipeserver.h"

#include "../common/proto.h"
#include "../common/pipe_util.h"
#include "../common/settings.h"
#include "../common/log.h"

namespace lingxi {

constexpr uint32_t kDebounceMs = 120;   // 预测防抖
constexpr size_t kMaxQueue = 8;         // 有界队列

bool PipeServer::Start(CandWindow* wnd, IPredictor* engine, IPredictor* pinyinEngine)
{
    _wnd = wnd;
    _engine = engine;
    _pinyinEngine = pinyinEngine;
    InitializeCriticalSection(&_cs);
    _csInit = true;
    _hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    _hQueueSem = CreateSemaphoreW(NULL, 0, 64, NULL);

    _wnd->SetPickHandler([this](uint32_t ver, uint32_t idx) {
        // 鼠标点击：先转发给宿主上屏，再作为选字反馈喂给拼音引擎自学习
        ClientCtx* a = nullptr;
        EnterCriticalSection(&_cs);
        a = _active;
        LeaveCriticalSection(&_cs);
        SendPick(ver, idx);
        if (a) ApplyLearn(a, ver, idx);
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
    if (_hPredictThread)
    {
        WaitForSingleObject(_hPredictThread, 2000);
        CloseHandle(_hPredictThread);
        _hPredictThread = NULL;
    }
    if (_hThread)
    {
        WaitForSingleObject(_hThread, 2000);
        CloseHandle(_hThread);
        _hThread = NULL;
    }
    if (_hQueueSem) { CloseHandle(_hQueueSem); _hQueueSem = NULL; }
    if (_csInit) { DeleteCriticalSection(&_cs); _csInit = false; }
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
        auto* c = new ClientCtx();
        c->hPipe = h;
        c->owner = this;
        HANDLE t = CreateThread(NULL, 0, s_ClientProc, c, 0, NULL);
        if (t) CloseHandle(t);
        else { CloseHandle(h); delete c; }
    }
}

DWORD WINAPI PipeServer::s_ClientProc(LPVOID p)
{
    auto* c = (ClientCtx*)p;
    c->owner->ClientProc(c);
    return 0;
}

void PipeServer::ClientProc(ClientCtx* c)
{
    for (;;)
    {
        uint8_t type = 0;
        std::vector<uint8_t> body;
        bool rf = ReadFrame(c->hPipe, _hStop, type, body);
        LX_LOG(L"client", L"ReadFrame pipe=%p ok=%d type=%u body=%u",
               c->hPipe, (int)rf, (uint32_t)type, (uint32_t)body.size());
        if (!rf) break;
        HandleMsg(c, type, body);
    }

    // 标记死亡并摘除活动标记（预测工作线程据此丢弃结果）
    c->alive = false;
    bool wasActive = false;
    EnterCriticalSection(&_cs);
    wasActive = (_active == c);
    c->active = false;
    if (wasActive) _active = nullptr;
    LeaveCriticalSection(&_cs);
    if (wasActive) _wnd->Hide();

    CloseHandle(c->hPipe);
    LX_LOG(L"ui", L"宿主客户端断开");
    delete c;
}

void PipeServer::HandleMsg(ClientCtx* c, uint8_t type, const std::vector<uint8_t>& body)
{
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();

    switch (type)
    {
    case MSG_HELLO:
    {
        MsgHello m{};
        if (ReadPod(p, end, m))
            c->dark = m.darkTheme != 0;
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
            _wnd->SetPage(pg.ver, pg.pageStart);
        break;
    }
    case MSG_PICKED:
    {
        MsgPick pk{}; // 复用 ver+index 结构
        if (ReadPod(p, end, pk))
            ApplyLearn(c, pk.ver, pk.index);
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

void PipeServer::Activate(ClientCtx* c)
{
    EnterCriticalSection(&_cs);
    _active = c;
    c->active = true;
    LeaveCriticalSection(&_cs);
    LX_LOG(L"ui", L"客户端激活 pipe=%p", c->hPipe);
}

void PipeServer::Deactivate(ClientCtx* c)
{
    bool wasActive = false;
    EnterCriticalSection(&_cs);
    wasActive = (_active == c);
    c->active = false;
    if (wasActive) _active = nullptr;
    LeaveCriticalSection(&_cs);
    if (wasActive) _wnd->Hide();
}

void PipeServer::HandleCtx(ClientCtx* c, const std::vector<uint8_t>& body)
{
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();
    MsgCtx m{};
    if (!ReadPod(p, end, m)) return;
    std::wstring text;
    if (!ReadStr(p, end, text)) return;

    LX_LOG(L"pipe", L"CTX ver=%u chars=%u flags=%u",
           m.ver, (uint32_t)text.size(), (uint32_t)m.flags);

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
    EnqueuePredict(c, m.ver, text, c->dark, rc, (m.flags & CTX_F_PINYIN) != 0);
}

void PipeServer::EnqueuePredict(ClientCtx* c, uint32_t ver, const std::wstring& text,
                                bool dark, const RECT& caret, bool pinyin)
{
    PendingPredict e;
    e.c = c;
    e.ver = ver;
    e.text = text;
    e.dark = dark;
    e.caret = caret;
    e.pinyin = pinyin;
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
        // 取最新请求 + 120ms 防抖合并（打字是连续事件，等一等收后面的更新）
        for (;;)
        {
            {
                EnterCriticalSection(&_cs);
                if (!_queue.empty())
                {
                    e = _queue.back();
                    have = true;
                    _queue.clear(); // 更旧的请求一律丢弃
                }
                LeaveCriticalSection(&_cs);
            }
            if (!have) break;
            uint64_t age = GetTickCount64() - e.enqueuedAt;
            if (age >= kDebounceMs) break;
            Sleep((DWORD)(kDebounceMs - age));
            // 睡醒后回到循环顶重新取：若期间有新请求则处理新的
        }
        if (!have) continue;
        if (WaitForSingleObject(_hStop, 0) != WAIT_TIMEOUT) break;

        // 预测期间不持任何锁（引擎可能耗时 300ms）
        // 拼音模式走专用引擎（组合字母串→汉字），否则走上下文降级链
        uint64_t tPred = GetTickCount64();
        Prediction pred = (e.pinyin && _pinyinEngine)
                              ? _pinyinEngine->Predict(e.text)
                              : _engine->Predict(e.text);
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
        size_t cap = e.pinyin ? (size_t)kMaxCandidates : (size_t)st.candidateCount;
        if (pred.cands.size() > cap)
            pred.cands.resize(cap);
        LX_LOG(L"pipe", L"裁剪后候选=%u paused=%d enComp=%d pinyin=%d",
               (uint32_t)pred.cands.size(), (int)st.paused, (int)st.enCompletion,
               (int)e.pinyin);

        {
            EnterCriticalSection(&_cs);
            _active = e.c;
            e.c->active = true;
            LeaveCriticalSection(&_cs);
        }
        bool dark = (st.darkMode == 0) ? e.dark : (st.darkMode == 2);
        if (!pred.cands.empty())
            _wnd->Show(e.caret, pred.cands, dark, e.ver);
        else
            _wnd->Hide();
        // 记录最近一次拼音预测供选字反馈自学习（临界区保护，跨线程读取）
        if (e.pinyin && !pred.cands.empty())
        {
            EnterCriticalSection(&_cs);
            e.c->lastLearn.ver = e.ver;
            e.c->lastLearn.raw = e.text;
            e.c->lastLearn.cands = pred.cands;
            LeaveCriticalSection(&_cs);
        }
        SendCandidates(e.c, e.ver, pred);
    }
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
    WriteFrame(c->hPipe, MSG_CANDS, bodyOut.data(), (uint32_t)bodyOut.size(), 500);
}

void PipeServer::SendPick(uint32_t ver, uint32_t index)
{
    ClientCtx* a = nullptr;
    EnterCriticalSection(&_cs);
    a = _active;
    LeaveCriticalSection(&_cs);
    if (!a || !a->alive) return;

    MsgPick m{ ver, index };
    WriteFrame(a->hPipe, MSG_PICK, &m, sizeof(m), 200);
    LX_LOG(L"ui", L"PICK ver=%u index=%u", ver, index);
}

// 选字反馈（键盘/鼠标）：按 ver 取最近一次拼音预测，用全局索引定位被选词，
// 把（原始拼音串, 词）喂给拼音引擎做自学习。
void PipeServer::ApplyLearn(ClientCtx* c, uint32_t ver, uint32_t index)
{
    std::wstring raw, word;
    EnterCriticalSection(&_cs);
    if (c && c->lastLearn.ver == ver && index < c->lastLearn.cands.size())
    {
        raw = c->lastLearn.raw;
        word = c->lastLearn.cands[index];
    }
    LeaveCriticalSection(&_cs);
    if (!raw.empty() && !word.empty() && _pinyinEngine)
    {
        _pinyinEngine->Learn(raw, word);
        LX_LOG(L"pipe", L"自学习反馈 ver=%u idx=%u '%ls'", ver, index, word.c_str());
    }
}

} // namespace lingxi
