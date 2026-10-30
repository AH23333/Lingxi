//
// LingxiUI.exe —— 候选窗 + 预测引擎宿主进程。
// 单实例由命名管道 FILE_FLAG_FIRST_PIPE_INSTANCE 保证；重复启动静默退出。
// `LingxiUI.exe --probe-llm`：同步跑一次 LLM 预测并写日志（诊断用），随即退出。
//
#include <windows.h>

#include "window.h"
#include "pipeserver.h"
#include "settingsui.h"
#include "uiinternal.h" // g_hooks（P3 回调收口：装配段一次性填充）
#include "usagestats.h"
#include "hotphraseui.h"
#include "phraseui.h"
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
                                 // v 组合语义取证（#1 v 重设计）：v/vk/vlog 应 0 候选→CommitRaw
                                 L"v", L"vk", L"vlog",
                                 // 长纯声母简拼取证：gylg→供应链、gylgl→供应链管理、xyzxyz→零
                                 L"gyl", L"gylg", L"gylgl", L"xyzxyz",
                                 // B6 隔音符取证：xi'an（显式边界）vs xian（歧义串）
                                 L"xi'an", L"xian",
                                 // P4c 长句+尾残段（碎切分闸门后的候选构成基线）
                                 L"nichifanlemawohenb" };
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

// 续写重复取证：逐字（中文）/逐词（英文）模拟打字走真实装配链，dump 每步候选。
// 用于诊断「AI 补全词大量重复」三种形态：列表内重复 / 前缀堆叠 / 时间维重复。
static int ProbeTyping()
{
    LX_LOG(L"typing", L"probe: 开始（真实装配链逐字模拟，首次含模型加载）");
    lingxi::IPredictor* c = lingxi::CreatePredictorChain();
    auto step = [&](const std::wstring& ctx)
    {
        uint64_t t0 = GetTickCount64();
        lingxi::Prediction r = c->Predict(ctx);
        std::wstring joined;
        for (size_t k = 0; k < r.cands.size(); k++)
        {
            if (k) joined += L" || ";
            joined += L"[" + std::to_wstring((int)k) + L"]" + r.cands[k];
        }
        LX_LOG(L"typing", L"ctx='%ls' -> %u cands %ls (%llu ms)",
               ctx.c_str(), (uint32_t)r.cands.size(), joined.c_str(),
               (unsigned long long)(GetTickCount64() - t0));
    };
    // 中文逐字（从第 2 字起——空/单字上下文多为空候选）
    const wchar_t* zh = L"今天天气真不错，我们一起去公园散步吧";
    for (size_t i = 2; i <= wcslen(zh); i++)
        step(std::wstring(zh, i));
    // 英文逐词
    const wchar_t* en = L"Hello world, this is a nice day for ";
    {
        std::wstring acc;
        const wchar_t* p = en;
        while (*p)
        {
            const wchar_t* q = wcschr(p, L' ');
            if (!q) q = p + wcslen(p);
            acc.append(p, (size_t)(q - p));
            step(acc);
            if (!*q) break;
            acc += L' ';
            p = q + 1;
        }
    }
    delete c;
    LX_LOG(L"typing", L"probe: 结束");
    return 0;
}

// ===== 命令行信息类子命令的共享输出（--version/--help/--diagnostics） =====
// 三级输出：① 控制台标准句柄 → WriteConsoleW（交互 cmd 下中文不乱码）；
// ② 重定向句柄（管道/文件）→ UTF-16LE 直写（PowerShell -Encoding Unicode 读）；
// ③ 都没有（双击启动等 GUI 场景）→ 弹窗兜底。
void EmitCliText(const wchar_t* text)
{
    if (!text || !*text) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(hOut, text, (DWORD)wcslen(text), &written, NULL);
        return;
    }
    if (hOut && hOut != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(hOut, text, (DWORD)(wcslen(text) * sizeof(wchar_t)),
                  &written, NULL);
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        HANDLE hCon = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hCon != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteConsoleW(hCon, text, (DWORD)wcslen(text), &written, NULL);
            CloseHandle(hCon);
        }
        FreeConsole();
        return;
    }
    MessageBoxW(NULL, text, L"灵犀输入法", MB_OK);
}

// --help：命令行选项速查
const wchar_t* kHelpText =
    L"灵犀输入法命令行选项：\n"
    L"  --version      版本与构建日期\n"
    L"  --help         本帮助\n"
    L"  --diagnostics  系统与配置诊断报告（排障一屏汇总）\n"
    L"  --settings     打开设置页（已运行实例则转发后退出）\n"
    L"  --symbols      打开符号面板（已运行实例则转发后退出）\n"
    L"  --probe-pinyin / --probe-llm / --probe-chain / --probe-typing\n"
    L"                 内部探针（回归脚本用）\n";

// --diagnostics：OS/构建/DPI/设置/数据文件统计（排障一屏汇总）
std::wstring lingxi::BuildDiagnosticsText()
{
    std::wstring t = L"灵犀输入法 诊断报告\n";
    wchar_t buf[512];
    _snwprintf_s(buf, _TRUNCATE, L"构建日期：%hs\n", __DATE__);
    t += buf;
    { // OS 版本（注册表：Win10/11 一致存在）
        wchar_t build[32] = L"", disp[32] = L"";
        DWORD ubr = 0, cbUbr = sizeof(ubr);
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS)
        {
            DWORD cb = 32;
            RegQueryValueExW(hk, L"CurrentBuildNumber", NULL, NULL, (BYTE*)build, &cb);
            cb = 32;
            RegQueryValueExW(hk, L"DisplayVersion", NULL, NULL, (BYTE*)disp, &cb);
            RegQueryValueExW(hk, L"UBR", NULL, NULL, (BYTE*)&ubr, &cbUbr);
            RegCloseKey(hk);
        }
        _snwprintf_s(buf, _TRUNCATE, L"系统：Windows 10.0.%ls.%lu（%ls）\n",
                     build, (unsigned long)ubr, disp);
        t += buf;
    }
    { // 系统 DPI
        UINT dpi = 96;
        if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
            dpi = pGetDpi();
        _snwprintf_s(buf, _TRUNCATE, L"系统 DPI：%u（缩放 %u%%）\n",
                     dpi, (unsigned)(dpi * 100 / 96));
        t += buf;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf_s(buf, _TRUNCATE, L"可执行：%ls\n", exe);
    t += buf;
    lingxi::Settings s = lingxi::LoadSettings();
    _snwprintf_s(buf, _TRUNCATE,
        L"设置：拼音=%u 候选数=%u 字号=%u 字体=%u 皮肤=%u 不透明度=%u%% "
        L"双拼=%u 繁体=%u 暂停=%u 固定位置=%u\n",
        (unsigned)s.pinyinEnabled, (unsigned)s.candidateCount,
        (unsigned)s.candidateFontSize, (unsigned)s.uiFont, (unsigned)s.skinIndex,
        (unsigned)s.candidateOpacity, (unsigned)s.shuangpin,
        (unsigned)s.traditionalOutput, (unsigned)s.paused,
        (unsigned)s.candidatePinned);
    t += buf;
    // 数据文件统计（用户词典行数 / 热键短语条数）
    {
        wchar_t ap[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH) > 0)
        {
            std::wstring dp = std::wstring(ap) + L"\\LingxiIme\\userdict.txt";
            HANDLE h = CreateFileW(dp.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE)
            {
                char lb[8192];
                DWORD n = 0;
                long long lines = 0;
                bool any = false;
                while (ReadFile(h, lb, sizeof(lb), &n, NULL) && n)
                {
                    any = any || n > 0;
                    for (DWORD i = 0; i < n; i++)
                        if (lb[i] == '\n') lines++;
                }
                CloseHandle(h);
                _snwprintf_s(buf, _TRUNCATE, L"用户词典：%lld 条（%ls）\n",
                             any ? lines + 1 : 0, dp.c_str());
                t += buf;
            }
            else
            {
                _snwprintf_s(buf, _TRUNCATE, L"用户词典：尚无（%ls）\n", dp.c_str());
                t += buf;
            }
        }
        _snwprintf_s(buf, _TRUNCATE, L"热键短语：%zu 条\n",
                     lingxi::LoadHotPhrases().size());
        t += buf;
    }
    _snwprintf_s(buf, _TRUNCATE, L"打字统计：%ls\n",
                 lingxi::BuildStatsBrief().c_str());
    t += buf;
    return t;
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
    if (argv && argc >= 2 && wcscmp(argv[1], L"--version") == 0)
    {
        wchar_t msg[96];
        _snwprintf_s(msg, _TRUNCATE, L"灵犀输入法 1.0（构建于 %hs）\n", __DATE__);
        EmitCliText(msg);
        LocalFree(argv);
        return 0;
    }
    if (argv && argc >= 2 && wcscmp(argv[1], L"--help") == 0)
    {
        EmitCliText(kHelpText);
        LocalFree(argv);
        return 0;
    }
    if (argv && argc >= 2 && wcscmp(argv[1], L"--diagnostics") == 0)
    {
        // 诊断真实配置：清测试沙箱变量后读（与生产路径同口径）
        SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", NULL);
        EmitCliText(lingxi::BuildDiagnosticsText().c_str());
        LocalFree(argv);
        return 0;
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
    if (argv && argc >= 2 && wcscmp(argv[1], L"--probe-typing") == 0)
    {
        int rc = ProbeTyping();
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    // 生产实例拒绝设置沙箱：LINGXI_SETTINGS_NS 是 deep_test/e2e/engine_test 的
    // 进程级隔离变量，但测试宿主进程内的 DLL 会按需 spawn LingxiUI，使其继承
    // 测试环境——生产 UI 带着该变量会把设置页保存静默写进 LingxiIme_<ns>
    // 沙箱键，用户表现为"改了不生效"（2026-10-29 晚实测事故：19:40 的全部
    // 修改落进 _test 键）。probe 子命令保留沙箱（e2e 断言依赖），生产路径
    // 在此强制清除，确保设置 UI 永远读写真实 HKCU。
    SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", NULL);

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
    g_insertText = [&server](const std::wstring& s) { server.SendInsertText(s); };
    g_reqSymbols = [&] {
        lingxi::ShowSymbolPanel(hInst,
            [&server](const std::wstring& s) { server.SendInsertText(s); });
    };
    g_reqSettings = [&] { lingxi::ShowSettingsWindow(hInst); };
    // 候选窗工具条按钮（符/设）→ 与托盘/热键同入口；‹›翻页与固钉在窗口内部处理
    wnd.SetButtonHandler([&](lingxi::CandWindow::Btn b) {
        if (b == lingxi::CandWindow::Btn::Symbols) { if (g_reqSymbols) g_reqSymbols(); }
        else if (b == lingxi::CandWindow::Btn::Settings) { if (g_reqSettings) g_reqSettings(); }
    });
    // ---- 全局回调装配（P3 收口：一次性填充 lingxi::g_hooks，见 uiinternal.h）----
    // 消费点：托盘菜单/设置窗/词典窗/短语窗/备份导入，统一读 g_hooks.xxx。
    // 用户词典管理变更后让拼音引擎重新加载；R2 自定义短语与用户词典同一
    // 热载通道（phrases.txt 在同一次重载里装入）。
    if (pinyinEngine)
        lingxi::g_hooks.reloadUserDict = [&] { pinyinEngine->ReloadUserDict(); };
    lingxi::g_hooks.phrasesChanged = [&] { if (pinyinEngine) pinyinEngine->ReloadUserDict(); };
    lingxi::g_hooks.settingsChanged = syncHotkey;       // 设置保存后动态注册/注销热键
    lingxi::g_hooks.hotPhrasesChanged = syncHotPhrases; // 管理窗保存后重注册短语热键
    // 托盘菜单"符号面板"入口（复用 g_reqSymbols）；"固定候选窗位置"勾选项
    // 按窗口实际位置切换（固/拖拽同语义）。
    lingxi::g_hooks.showSymbols = [] { if (g_reqSymbols) g_reqSymbols(); };
    lingxi::g_hooks.pinToggle = [&] { wnd.TogglePin(); };
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

    lingxi::StatsFlush(); // 退出：未落盘的打字统计（≤20 次）补写
    server.Stop();
    wnd.Shutdown();
    delete engine;
    delete pinyinEngine;
    return 0;
}
