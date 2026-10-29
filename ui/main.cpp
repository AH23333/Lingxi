//
// LingxiUI.exe —— 候选窗 + 预测引擎宿主进程。
// 单实例由命名管道 FILE_FLAG_FIRST_PIPE_INSTANCE 保证；重复启动静默退出。
// `LingxiUI.exe --probe-llm`：同步跑一次 LLM 预测并写日志（诊断用），随即退出。
//
#include <windows.h>

#include "window.h"
#include "pipeserver.h"
#include "settingsui.h"
#include "hotphraseui.h"
#include "symbolpanel.h"
#include "engine/predictor.h"
#include "../common/log.h"
#include "../common/settings.h"

// ===== 单实例命令转发 =====
// 第二个实例（--symbols/--settings）不直接建窗，而是通过命名事件通知已运行
// 实例。这样即使宿主 TIP 已自动拉起一个无参数实例（它占了单实例），命令
// 仍能送达。
constexpr wchar_t kEvtShowSymbols[] = L"Local\\LingxiUI_ShowSymbols";
constexpr wchar_t kEvtShowSettings[] = L"Local\\LingxiUI_ShowSettings";
constexpr UINT WM_NOTIFY_SYMBOLS = WM_APP + 20;
constexpr UINT WM_NOTIFY_SETTINGS = WM_APP + 21;
constexpr int kHotkeySymbols = 1; // RegisterHotKey 热键 ID（Ctrl+Shift+B）
constexpr int kHotPhraseIdBase = 100; // 快捷短语热键 ID 基址（0..23 偏移）

std::function<void()> g_reqSymbols;
std::function<void()> g_reqSettings;
// 快捷短语：当前生效条目（仅 UI 线程读写——注册/保存/WM_HOTKEY 均在消息循环线程）
std::vector<lingxi::HotPhrase> g_hotPhrases;
std::function<void(const std::wstring&)> g_insertText; // 热键触发 → SendInsertText

LRESULT CALLBACK NotifyWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NOTIFY_SYMBOLS) { if (g_reqSymbols) g_reqSymbols(); return 0; }
    if (msg == WM_NOTIFY_SETTINGS) { LX_LOG(L"ui", L"WM_NOTIFY_SETTINGS 到达 req=%d", (int)!!g_reqSettings); if (g_reqSettings) g_reqSettings(); return 0; }
    if (msg == WM_HOTKEY)
    {
        int id = (int)wp;
        if (id == kHotkeySymbols) { if (g_reqSymbols) g_reqSymbols(); return 0; }
        // 快捷短语：id 映射 g_hotPhrases 下标，经既有插入链路上屏
        if (id >= kHotPhraseIdBase &&
            id < kHotPhraseIdBase + (int)g_hotPhrases.size())
        {
            LX_LOG(L"ui", L"快捷短语热键触发: %ls",
                   lingxi::FormatHotkey(g_hotPhrases[id - kHotPhraseIdBase].mods,
                                        g_hotPhrases[id - kHotPhraseIdBase].vk).c_str());
            if (g_insertText) g_insertText(g_hotPhrases[id - kHotPhraseIdBase].phrase);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

struct NotifyThreadParam { HANDLE evt; HWND target; UINT msg; };

DWORD WINAPI NotifyThreadProc(LPVOID p)
{
    auto* tp = (NotifyThreadParam*)p;
    for (;;)
    {
        if (WaitForSingleObject(tp->evt, INFINITE) == WAIT_OBJECT_0)
        {
            const BOOL okPost = PostMessageW(tp->target, tp->msg, 0, 0); // FORWARD-DIAG
            LX_LOG(L"ui", L"转发事件命中→Post msg=%u ok=%d gle=%u", tp->msg, okPost, GetLastError());
        }
    }
    return 0;
}

// 打开（必要时等待）命名事件并触发，供第二个实例调用。
// 遗留 P2 修复（cur/GLM_EXPLORATION_R2R5.md R5"顺手"条）：主实例持有单实例
// Mutex 但通知事件尚未创建（冷启动 LLM 加载可达数秒）时，4 秒等待耗尽不再
// 静默丢命令——弹一次提示让用户知道命令没送达（每次进程存活至多一次，
// --symbols + --settings 双超时也只弹一盒）。
static void SignalNamedEvent(const wchar_t* name)
{
    HANDLE evt = NULL;
    for (int i = 0; i < 40; i++)
    {
        evt = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (evt) break;
        Sleep(100); // 主实例可能还在启动
    }
    if (evt) { SetEvent(evt); CloseHandle(evt); return; }
    LX_LOG(L"ui", L"SignalNamedEvent: 4s 超时，主实例未就绪 (%ls)", name);
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        MessageBoxW(NULL,
            L"灵犀输入法主程序仍在启动中，请稍后重试。",
            L"灵犀输入法", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }
}

static int ProbeLlm()
{
    uint64_t t0 = GetTickCount64();
    LX_LOG(L"llm", L"probe: 开始");
    lingxi::IPredictor* llm = lingxi::CreateLlmPredictor();
    lingxi::Prediction pred = llm->Predict(L"今天天气");
    LX_LOG(L"llm", L"probe: %u 候选, 耗时 %llu ms",
           (uint32_t)pred.cands.size(), (unsigned long long)(GetTickCount64() - t0));
    for (auto& c : pred.cands)
        LX_LOG(L"llm", L"probe cand: %ls", c.c_str());

    // ---- M4.5 KV 增量复用验证：追加/相同 prompt 的延迟应显著低于首次 ----
    auto timed = [&](const wchar_t* ctx) {
        uint64_t t = GetTickCount64();
        lingxi::Prediction p = llm->Predict(ctx);
        uint64_t ms = GetTickCount64() - t;
        LX_LOG(L"llm", L"probe incr '%ls' -> %u cands, 首选'%ls', %llu ms",
               ctx, (uint32_t)p.cands.size(),
               p.cands.empty() ? L"" : p.cands[0].c_str(), (unsigned long long)ms);
        return p;
    };
    timed(L"今天天气不错");                          // 追加型增量
    lingxi::Prediction a = timed(L"今天天气不错，");   // 追加型增量
    lingxi::Prediction b = timed(L"今天天气不错，");   // 完全相同（1-token 重算路径）
    if (!a.cands.empty() && !b.cands.empty() && a.cands[0] == b.cands[0])
        LX_LOG(L"llm", L"probe incr 一致性: OK（相同 prompt 首选一致）");
    else
        LX_LOG(L"llm", L"probe incr 一致性: MISMATCH（增量路径有误，需排查）");
    delete llm;
    return 0;
}

// 拼音引擎诊断：常见输入的首选/候选列表写入日志（%TEMP%\lingxi-ime.log）
static int ProbePinyin()
{
    LX_LOG(L"pinyin", L"probe: 开始");
    lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
    LX_LOG(L"pinyin", L"probe: 引擎已加载");
    const wchar_t* inputs[] = { L"jintian", L"jintianqi", L"nihao", L"zhidao", L"ni hao", L"moran",
                                // B3 零声母简写取证：i/u 打头韵母（省略 y/w 的人类写法）
                                L"i", L"u", L"in", L"iu", L"ui", L"un", L"uo", L"ian",
                                // B6 隔音符取证：xi'an（显式边界）vs xian（歧义串）
                                L"xi'an", L"xian" };
    for (const wchar_t* in : inputs)
    {
        uint64_t t0 = GetTickCount64();
        lingxi::Prediction pred = py->Predict(in);
        std::wstring joined;
        for (size_t k = 0; k < pred.cands.size(); k++)
        {
            if (k) joined += L" | ";
            joined += pred.cands[k];
        }
        LX_LOG(L"pinyin", L"probe '%ls' -> [%ls] (%u cands, %llu ms)",
               in, joined.c_str(), (uint32_t)pred.cands.size(),
               (unsigned long long)(GetTickCount64() - t0));
    }
    delete py;
    return 0;
}

// B4 端到端取证：真实装配链（LLM+词表）在英文/中文上下文下的路由与候选纯度。
// --probe-llm 直连 LlmPredictor 不经链路由，纯 ASCII 过滤/词表优先只能在此验证。
static int ProbeChain()
{
    LX_LOG(L"chain", L"probe: 开始（真实装配链 LLM+词表）");
    lingxi::IPredictor* c = lingxi::CreatePredictorChain();
    const wchar_t* ctxs[] = {
        L"今天天气",           // 中文上下文：LLM 优先
        L"Today is a nice ",  // 纯 ASCII 词边界：词表优先，LLM 兜底须过滤为 ASCII
        L"inte",              // 词中：词表补全
        L"hello wor",         // 词中：词表补全
    };
    for (const wchar_t* ctx : ctxs)
    {
        uint64_t t0 = GetTickCount64();
        lingxi::Prediction r = c->Predict(ctx);
        std::wstring joined;
        bool ascii = true;
        for (size_t k = 0; k < r.cands.size(); k++)
        {
            if (k) joined += L" | ";
            joined += r.cands[k];
            for (wchar_t ch : r.cands[k])
                if (ch >= 0x80) ascii = false;
        }
        LX_LOG(L"chain", L"probe '%ls' -> [%ls] (%u cands, ascii=%d, %llu ms)",
               ctx, joined.c_str(), (uint32_t)r.cands.size(), (int)ascii,
               (unsigned long long)(GetTickCount64() - t0));
    }
    delete c;
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    lingxi::InitLogging(); // 按 LINGXI_LOG 环境变量开启热路径诊断日志
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool wantSettings = false;
    bool wantSymbols = false;
    if (argv)
    {
        for (int i = 1; i < argc; i++)
        {
            if (wcscmp(argv[i], L"--settings") == 0) wantSettings = true;
            if (wcscmp(argv[i], L"--symbols") == 0) wantSymbols = true;
        }
    }
    if (argv && argc >= 2 && wcscmp(argv[1], L"--probe-llm") == 0)
    {
        int rc = ProbeLlm();
        LocalFree(argv);
        return rc;
    }
    if (argv && argc >= 2 && wcscmp(argv[1], L"--probe-pinyin") == 0)
    {
        int rc = ProbePinyin();
        LocalFree(argv);
        return rc;
    }
    if (argv && argc >= 2 && wcscmp(argv[1], L"--probe-chain") == 0)
    {
        int rc = ProbeChain();
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    // 早期单实例：任何窗口/引擎/LLM 加载前先占命名 Mutex，重复实例立即退出。
    // 旧方案靠 pipe 的 FILE_FLAG_FIRST_PIPE_INSTANCE 检测，时机太晚——重复实例
    // 已完整加载 LLM、创建托盘后才退出，造成资源浪费与多实例竞争。
    HANDLE hSingleton = CreateMutexW(NULL, TRUE, L"Local\\LingxiImeSingleton");
    if (!hSingleton || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hSingleton) CloseHandle(hSingleton);
        // 已有实例：把命令转发给它，再退出
        if (wantSymbols) SignalNamedEvent(kEvtShowSymbols);
        if (wantSettings) SignalNamedEvent(kEvtShowSettings);
        return 0;
    }

    // 就绪信号：进程启动即创建未触发事件，管道服务监听后置信号。
    // e2e 等此事件替代盲等（冷启动 LLM 加载可达数秒，管道未就绪时 CTX 被丢弃）。
    HANDLE hReady = CreateEventW(NULL, TRUE, FALSE, L"Local\\LingxiUIReady");

    lingxi::CandWindow wnd;
    if (!wnd.Init(hInst))
        return 1;

    // 共享 LLM 实例：同时用于续写链与拼音候选重排（只加载一次模型）
    lingxi::IPredictor* sharedLlm = lingxi::CreateLlmPredictor();
    lingxi::ILlmReranker* reranker = nullptr;
    if (sharedLlm)
        reranker = dynamic_cast<lingxi::ILlmReranker*>(sharedLlm);
    // 预测降级链：LLM（共享）→ 词表引擎（永远可用）；chain 持有 sharedLlm 所有权
    lingxi::IPredictor* engine = lingxi::CreatePredictorChain(sharedLlm);
    // M5 拼音引擎（zh-CN profile 组合模式下使用；数据缺失时为 null，拼音直拼兜底）
    lingxi::IPredictor* pinyinEngine = lingxi::CreatePinyinPredictor();

    lingxi::PipeServer server;
    if (!server.Start(&wnd, engine, pinyinEngine, reranker))
        return 1; // 已有实例在运行（或内部错误）

    if (hReady) SetEvent(hReady); // 管道服务已监听；句柄随进程存活，不主动关

    LX_LOG(L"ui", L"LingxiUI 启动完成");
    // 用户词典管理变更后让拼音引擎重新加载
    if (pinyinEngine)
        lingxi::SetUserDictReloadCallback([&] { pinyinEngine->ReloadUserDict(); });
    lingxi::InitTrayAndSettings(hInst); // 托盘 + 设置入口（右下角）

    // 单实例命令转发：命名事件（自动重置）+ 隐藏通知窗口 + 等待线程
    HANDLE hEvtSymbols = CreateEventW(NULL, FALSE, FALSE, kEvtShowSymbols);
    HANDLE hEvtSettings = CreateEventW(NULL, FALSE, FALSE, kEvtShowSettings);
    WNDCLASSW nwc{};
    nwc.lpfnWndProc = NotifyWndProc;
    nwc.hInstance = hInst;
    nwc.lpszClassName = L"LingxiNotifyWnd";
    RegisterClassW(&nwc);
    HWND hNotify = CreateWindowExW(0, L"LingxiNotifyWnd", L"", 0,
                                   0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    // 全局热键 Ctrl+Shift+B 调出符号面板；按设置注册/注销（关闭则不独占组合键）
    auto syncHotkey = [hNotify] {
        if (lingxi::LoadSettings().symbolHotkey)
            RegisterHotKey(hNotify, kHotkeySymbols,
                           MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, (UINT)'B');
        else
            UnregisterHotKey(hNotify, kHotkeySymbols);
    };
    syncHotkey();
    lingxi::SetSettingsChangedCallback(syncHotkey); // 设置保存后动态注册/注销
    // 快捷短语热键：全量注销→重读文件→逐条注册（失败=组合键被占用，记日志继续）
    auto syncHotPhrases = [hNotify] {
        for (size_t i = 0; i < lingxi::kMaxHotPhrases; i++)
            UnregisterHotKey(hNotify, kHotPhraseIdBase + (int)i);
        g_hotPhrases = lingxi::LoadHotPhrases();
        int failed = 0;
        for (size_t i = 0; i < g_hotPhrases.size(); i++)
        {
            if (!RegisterHotKey(hNotify, kHotPhraseIdBase + (int)i,
                                g_hotPhrases[i].mods | MOD_NOREPEAT, g_hotPhrases[i].vk))
            {
                failed++;
                LX_LOG(L"ui", L"快捷短语热键注册失败(占用): %ls",
                       lingxi::FormatHotkey(g_hotPhrases[i].mods, g_hotPhrases[i].vk).c_str());
            }
        }
        if (failed)
            LX_LOG(L"ui", L"快捷短语热键 %d 个注册失败（组合键被其他程序占用）", failed);
    };
    syncHotPhrases();
    lingxi::SetHotPhrasesChangedCallback(syncHotPhrases); // 管理窗保存后重注册
    g_insertText = [&server](const std::wstring& s) { server.SendInsertText(s); };
    g_reqSymbols = [&] {
        lingxi::ShowSymbolPanel(hInst,
            [&server](const std::wstring& s) { server.SendInsertText(s); });
    };
    g_reqSettings = [&] { lingxi::ShowSettingsWindow(hInst); };
    // 托盘菜单"符号面板"入口（复用 g_reqSymbols）
    lingxi::SetShowSymbolsCallback([] { if (g_reqSymbols) g_reqSymbols(); });
    static NotifyThreadParam tpS{ hEvtSymbols, hNotify, WM_NOTIFY_SYMBOLS };
    static NotifyThreadParam tpE{ hEvtSettings, hNotify, WM_NOTIFY_SETTINGS };
    CreateThread(NULL, 0, NotifyThreadProc, &tpS, 0, NULL);
    CreateThread(NULL, 0, NotifyThreadProc, &tpE, 0, NULL);

    if (wantSettings) g_reqSettings();
    if (wantSymbols) g_reqSymbols();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    server.Stop();
    wnd.Shutdown();
    delete engine;
    delete pinyinEngine;
    return 0;
}
