// pf_shot —— UI 走查截图工具：以假客户端触发各界面状态，留窗口给截图取证。
//   pf_shot cand   拼音候选第一页（带回显条）
//   pf_shot page   翻到第二页
//   pf_shot en     英文续写长候选
//   pf_shot toast  中/英模式气泡
#include "../common/proto.h"
#include "../common/pipe_util.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace lingxi;

static uint32_t g_ver = 900;

static bool SendCtxH(HANDLE h, const std::wstring& text, const std::wstring& ctx, uint8_t flags)
{
    MsgCtx m{};
    m.ver = ++g_ver;
    m.caretX = 620; m.caretY = 500; m.caretW = 2; m.caretH = 18;
    m.ctxChars = (uint32_t)ctx.size();
    m.textChars = (uint32_t)text.size();
    if (!ctx.empty()) flags |= CTX_F_CTX_PREFIX;
    m.flags = flags;
    std::vector<uint8_t> body;
    AppendPod(body, m);
    if (!ctx.empty()) AppendStr(body, ctx);
    AppendStr(body, text);
    return WriteFrame(h, MSG_CTX, body.data(), (uint32_t)body.size(), 500);
}

static void Drain(std::vector<uint8_t>& rxbuf, HANDLE h, HANDLE hStop)
{
    for (int i = 0; i < 128; i++)
    {
        DWORD av = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &av, NULL) || av == 0) break;
        uint8_t type = 0; std::vector<uint8_t> body;
        if (!ReadFrame(h, hStop, rxbuf, type, body)) break;
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring mode = argc > 1 ? argv[1] : L"cand";
    FILE* lg = fopen("tmp/pf_shot.log", "a");
    if (lg) { fprintf(lg, "start mode=%ls\n", mode.c_str()); fflush(lg); }
    HANDLE h = OpenLingxiPipeClient();
    if (h == INVALID_HANDLE_VALUE) { if (lg) { fprintf(lg, "pipe fail\n"); fclose(lg);} return 2; }
    MsgHello hello{ kProtoVer, GetCurrentProcessId(), 0 };
    WriteFrame(h, MSG_HELLO, &hello, sizeof(hello), 500);
    WriteFrame(h, MSG_ACTIVATE, NULL, 0, 500);
    Sleep(250);
    std::vector<uint8_t> rxbuf;
    HANDLE hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    Drain(rxbuf, h, hStop);

    if (mode == L"toast")
    {
        uint8_t en = 1;
        WriteFrame(h, MSG_MODE_TOAST, &en, 1, 300);
        Sleep(2500);
    }
    else if (mode == L"en")
    {
        SendCtxH(h, L"moreX international road trip", L"", 0);
        Sleep(4000);   // 等续写生成
        Drain(rxbuf, h, hStop);
        Sleep(60000);  // 留窗口给截图
    }
    else
    {
        // 拼音串 jinwantianshanghenhao（今天天气很好类长句，>9 候选可翻页）
        SendCtxH(h, L"jinwantianshanghenhaode", L"\x4ECA\x5929\x5929\x6C14", CTX_F_PINYIN);
        Sleep(1200);
        Drain(rxbuf, h, hStop);
        if (mode == L"page")
        {
            MsgPage pg{ g_ver, 9 };
            WriteFrame(h, MSG_PAGE, &pg, sizeof(pg), 300);
        }
        Sleep(60000);
    }
    WriteFrame(h, MSG_DEACTIVATE, NULL, 0, 200);
    WriteFrame(h, MSG_HIDE, NULL, 0, 200);
    CloseHandle(h);
    return 0;
}
