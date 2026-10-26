//
// 灵犀输入法预测引擎单元测试。
//
// 直接链接 ui/engine/ngram.cpp（不经 TSF/IPC/LingxiUI），对
// CreateDefaultPredictor() 做纯进程内断言：英文前缀补全、带上下文补全、
// 中文高频接续、空上下文行为与预测延迟（平均 < 5ms）。
//
// 数据定位：ngram.cpp 用 GetModuleFileName(NULL) 拼 <exe目录>\data\*.txt。
// 本测试 exe 输出在 bin\enginetest\，而词表随主构建放在 bin\Debug\data，
// 因此首次运行若 <exe目录>\data 缺词表，自动从 <exe目录>\..\Debug\data 拷贝。
//
#include "predictor.h"

#include <windows.h>
#include <cstdio>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

static void Report(bool ok, const char* name, const wchar_t* detail = L"")
{
    wprintf(L"[%s] %hs %s\n", ok ? L"PASS" : L"FAIL", name, detail);
    if (ok) g_pass++; else g_fail++;
}

// exe 所在目录（不带尾反斜杠）
static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return L".";
    std::wstring s(path);
    size_t pos = s.find_last_of(L'\\');
    return (pos == std::wstring::npos) ? L"." : s.substr(0, pos);
}

// 保证 <exe目录>\data\{en,zh}_words.txt 就位（首次运行从 bin\Debug\data 拷贝）
static bool EnsureWordlists()
{
    std::wstring exeDir = ExeDir();
    std::wstring dstDir = exeDir + L"\\data";
    std::wstring marker = dstDir + L"\\en_words.txt";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    std::wstring srcDir = exeDir + L"\\..\\Debug\\data";
    CreateDirectoryW(dstDir.c_str(), NULL);
    bool copiedEn = false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((srcDir + L"\\*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring from = srcDir + L"\\" + fd.cFileName;
            std::wstring to = dstDir + L"\\" + fd.cFileName;
            if (CopyFileW(from.c_str(), to.c_str(), FALSE) &&
                _wcsicmp(fd.cFileName, L"en_words.txt") == 0)
                copiedEn = true;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return copiedEn && GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct LatencyStats
{
    double avgMs = 0.0;
    double maxMs = 0.0;
};

// GetTickCount64 粒度约 15.6ms：平均值按"runs 次总耗时 / runs"计算（精确），
// 最大值取单次差分（受计时粒度限制，仅作参考）。
static LatencyStats MeasureLatency(lingxi::IPredictor* p, const std::wstring& ctx, int runs)
{
    LatencyStats st;
    p->Predict(ctx); // 预热（首调走冷路径）
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG maxDelta = 0;
    for (int i = 0; i < runs; i++)
    {
        ULONGLONG c0 = GetTickCount64();
        p->Predict(ctx);
        ULONGLONG d = GetTickCount64() - c0;
        if (d > maxDelta) maxDelta = d;
    }
    st.avgMs = (double)(GetTickCount64() - t0) / (double)runs;
    st.maxMs = (double)maxDelta;
    return st;
}

int wmain()
{
    if (EnsureWordlists())
        wprintf(L"[INFO] wordlists staged under <exeDir>\\data\n");
    else
        wprintf(L"[WARN] wordlists not staged into <exeDir>\\data; dict tests will fail\n");

    lingxi::IPredictor* p = lingxi::CreateDefaultPredictor();
    if (!p) { Report(false, "CreateDefaultPredictor"); return 1; }

    // 1. 英文前缀补全：候选非空，且 replaceLen 必须等于已输入前缀长度
    {
        lingxi::Prediction r = p->Predict(L"mor");
        wchar_t detail[160];
        bool ok = !r.cands.empty() && r.replaceLen == 3;
        _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls' replaceLen=%u",
                     r.cands.size(),
                     r.cands.empty() ? L"" : r.cands[0].c_str(), r.replaceLen);
        Report(ok, "en prefix predict: mor", detail);
    }

    // 2. 带上下文补全："good inte" 的首候选应以 inte 开头（不区分大小写）
    {
        lingxi::Prediction r = p->Predict(L"good inte");
        wchar_t detail[160];
        bool ok = !r.cands.empty() && _wcsnicmp(r.cands[0].c_str(), L"inte", 4) == 0;
        _snwprintf_s(detail, _TRUNCATE, L"first='%ls'",
                     r.cands.empty() ? L"" : r.cands[0].c_str());
        Report(ok, "en completion: good inte", detail);
    }

    // 3. 中文高频接续："今天天气不错" 应给出非空候选
    {
        lingxi::Prediction r = p->Predict(L"今天天气不错");
        wchar_t detail[160];
        _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls'",
                     r.cands.size(),
                     r.cands.empty() ? L"" : r.cands[0].c_str());
        Report(!r.cands.empty(), "zh continuation: 今天天气不错", detail);
    }

    // 4. 空上下文：不产生任何候选
    {
        lingxi::Prediction r = p->Predict(L"");
        Report(r.cands.empty(), "empty context: no candidates");
    }

    // 5. 延迟统计：三个上下文各跑 50 次，平均 < 5ms
    {
        const wchar_t* ctxs[] = { L"hello wor", L"今天", L"international" };
        bool allOk = true;
        for (const wchar_t* ctx : ctxs)
        {
            LatencyStats st = MeasureLatency(p, ctx, 50);
            wchar_t line[192];
            _snwprintf_s(line, _TRUNCATE, L"ctx='%ls' avg=%.3fms max=%.0fms",
                         ctx, st.avgMs, st.maxMs);
            wprintf(L"[INFO] latency %ls\n", line);
            if (st.avgMs >= 5.0) allOk = false;
        }
        Report(allOk, "latency avg < 5ms (50 runs x 3 contexts)");
    }

    delete p;

    wprintf(L"=== 结果: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
