//
// LingxiUI.exe —— 候选窗 + 预测引擎宿主进程。
// 单实例由命名管道 FILE_FLAG_FIRST_PIPE_INSTANCE 保证；重复启动静默退出。
// `LingxiUI.exe --probe-llm`：同步跑一次 LLM 预测并写日志（诊断用），随即退出。
//
#include <windows.h>

#include "window.h"
#include "pipeserver.h"
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
    if (argv) LocalFree(argv);

    lingxi::CandWindow wnd;
    if (!wnd.Init(hInst))
        return 1;

    // 预测降级链：LLM（若 llama.dll/模型就绪）→ 词表引擎（永远可用）
    lingxi::IPredictor* engine = lingxi::CreatePredictorChain();

    lingxi::PipeServer server;
    if (!server.Start(&wnd, engine))
        return 1; // 已有实例在运行（或内部错误）

    LX_LOG(L"ui", L"LingxiUI 启动完成");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    server.Stop();
    wnd.Shutdown();
    delete engine;
    return 0;
}
