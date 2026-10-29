// b3_probe —— B3 取证：首字母 i/u/v 的引擎行为现状（临时工具，不入 sln）
#include "../ui/engine/predictor.h"
#include <windows.h>
#include <cstdio>
#include <string>

static FILE* g_out = nullptr;

static void Dump(lingxi::IPredictor* py, const wchar_t* in)
{
    lingxi::Prediction r = py->Predict(in);
    std::wstring head;
    for (size_t i = 0; i < r.cands.size() && i < 6; i++)
    {
        if (i) head += L" | ";
        head += r.cands[i];
    }
    if (head.empty()) head = L"(空)";
    std::string inU8, headU8;
    int n1 = WideCharToMultiByte(CP_UTF8, 0, in, -1, nullptr, 0, nullptr, nullptr);
    inU8.resize(n1); WideCharToMultiByte(CP_UTF8, 0, in, -1, &inU8[0], n1, nullptr, nullptr);
    int n2 = WideCharToMultiByte(CP_UTF8, 0, head.c_str(), -1, nullptr, 0, nullptr, nullptr);
    headU8.resize(n2); WideCharToMultiByte(CP_UTF8, 0, head.c_str(), -1, &headU8[0], n2, nullptr, nullptr);
    fprintf(g_out, "[B3] '%s' -> %zu cands: %s\n",
            inU8.c_str(), r.cands.size(), headU8.c_str());
    fflush(g_out);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc > 2) { } // 词表定位沿用 engine_test 的 staging 规则：<exeDir>\data
    g_out = _wfopen(L"tmp/b3_out.txt", L"wb");
    if (!g_out) g_out = stdout;
    lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
    if (!py) { fprintf(g_out, "[B3] 引擎加载失败（data 未就位？）\n"); return 2; }
    Dump(py, L"i");
    Dump(py, L"u");
    Dump(py, L"v");
    Dump(py, L"haoyi");
    Dump(py, L"pi");
    Dump(py, L"niu");
    Dump(py, L"nv");
    Dump(py, L"lv");
    Dump(py, L"ia");
    Dump(py, L"ua");
    Dump(py, L"ve");
    Dump(py, L"yi");
    Dump(py, L"wu");
    delete py;
    return 0;
}
