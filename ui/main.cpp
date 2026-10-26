//
// LingxiUI.exe —— 候选窗 + 预测引擎宿主进程。
// 单实例由命名管道 FILE_FLAG_FIRST_PIPE_INSTANCE 保证；重复启动静默退出。
//
#include <windows.h>

#include "window.h"
#include "pipeserver.h"
#include "engine/predictor.h"
#include "../common/log.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
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
