//
// LingxiUI.exe —— 候选窗 + 预测引擎宿主进程。
// 单实例由命名管道 FILE_FLAG_FIRST_PIPE_INSTANCE 保证；重复启动静默退出。
// `LingxiUI.exe --probe-llm`：同步跑一次 LLM 预测并写日志（诊断用），随即退出。
//
#include <windows.h>

#include "window.h"
#include "pipeserver.h"
#include "settingsui.h"
#include "engine/predictor.h"
#include "../common/log.h"

static int ProbeLlm()
{
    uint64_t t0 = GetTickCount64();
    LX_LOG(L"llm", L"probe: 开始");
    lingxi::IPredictor* llm = lingxi::CreateLlmPredictor();
    lingxi::Prediction pred = llm->Predict(L"今天天气不错，");
    LX_LOG(L"llm", L"probe: %u 候选, 耗时 %llu ms",
           (uint32_t)pred.cands.size(), (unsigned long long)(GetTickCount64() - t0));
    for (auto& c : pred.cands)
        LX_LOG(L"llm", L"probe cand: %ls", c.c_str());
    delete llm;
    return 0;
}

// 拼音引擎诊断：常见输入的首选/候选列表写入日志（%TEMP%\lingxi-ime.log）
static int ProbePinyin()
{
    LX_LOG(L"pinyin", L"probe: 开始");
    lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
    LX_LOG(L"pinyin", L"probe: 引擎已加载");
    const wchar_t* inputs[] = { L"jintian", L"jintianqi", L"nihao", L"zhidao", L"ni hao", L"moran" };
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
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
    if (argv) LocalFree(argv);

    // 就绪信号：进程启动即创建未触发事件，管道服务监听后置信号。
    // e2e 等此事件替代盲等（冷启动 LLM 加载可达数秒，管道未就绪时 CTX 被丢弃）。
    HANDLE hReady = CreateEventW(NULL, TRUE, FALSE, L"Local\\LingxiUIReady");

    lingxi::CandWindow wnd;
    if (!wnd.Init(hInst))
        return 1;

    // 预测降级链：LLM（若 llama.dll/模型就绪）→ 词表引擎（永远可用）
    lingxi::IPredictor* engine = lingxi::CreatePredictorChain();
    // M5 拼音引擎（zh-CN profile 组合模式下使用；数据缺失时为 null，拼音直拼兜底）
    lingxi::IPredictor* pinyinEngine = lingxi::CreatePinyinPredictor();

    lingxi::PipeServer server;
    if (!server.Start(&wnd, engine, pinyinEngine))
        return 1; // 已有实例在运行（或内部错误）

    if (hReady) SetEvent(hReady); // 管道服务已监听；句柄随进程存活，不主动关

    LX_LOG(L"ui", L"LingxiUI 启动完成");
    lingxi::InitTrayAndSettings(hInst); // 托盘 + 设置入口（右下角）

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
