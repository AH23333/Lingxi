//
// 打字统计窗（托盘 2017 / 设置·词典页入口）。原 settingsui.cpp 统计窗段
// 拆出（P1 重构）。只读正文（BuildStatsText）+ 底部按钮条：刷新 / 清零
// （确认）/ 导出诊断报告（诊断全文+统计 → APPDATA lingxi-diag-时间戳.txt，
// UTF-8 BOM）。Esc 双路关窗（正文子类化 + 窗口本身）。
//
#include "uiinternal.h"
#include "settingsui.h" // BuildDiagnosticsText（main.cpp 实现，导出诊断报告复用）

#include <windows.h>
#include <string>

#include "../common/log.h"
#include "usagestats.h"

namespace lingxi {

namespace {

HWND g_hStats = nullptr;
HWND g_hStatsEdit = nullptr;
HFONT g_statsFont = nullptr;
WNDPROC g_oldStatsEditProc = nullptr; // 正文 EDIT 子类化（Esc 关窗）
constexpr int kStatsBarH = 48;        // 底部按钮条高
constexpr int IDC_STATS_REFRESH = 360;
constexpr int IDC_STATS_RESET = 361;
constexpr int IDC_STATS_EXPORT = 362;

void StatsRefreshText()
{
    if (g_hStatsEdit) SetWindowTextW(g_hStatsEdit, BuildStatsText().c_str());
}

// 导出诊断报告：诊断全文 + 统计正文 → APPDATA\LingxiIme\lingxi-diag-<时间戳>.txt
// （排障时可分享的完整文本文件；\n 统一转 \r\n、UTF-8 带 BOM，记事本直开不乱码）
void StatsExportReport(HWND h)
{
    std::wstring crlf;
    {
        std::wstring wtext = BuildDiagnosticsText();
        crlf.reserve(wtext.size() + 16);
        for (wchar_t ch : wtext)
        {
            if (ch == L'\n') crlf += L'\r';
            crlf += ch;
        }
    }
    crlf += L"\r\n打字统计：\r\n";
    crlf += BuildStatsText();
    std::string u8 = "\xEF\xBB\xBF"; // BOM
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(),
                                    NULL, 0, NULL, NULL);
        std::string s((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(),
                                &s[0], n, NULL, NULL);
        u8 += s;
    }
    std::wstring path = L"lingxi-diag.txt"; // APPDATA 缺失时的兜底（当前目录）
    wchar_t ap[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH) > 0)
    {
        std::wstring dir = std::wstring(ap) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"\\lingxi-diag-%04u%02u%02u-%02u%02u%02u.txt",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        path = dir + name;
    }
    bool ok = false;
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ok = WriteFile(hf, u8.data(), (DWORD)u8.size(), &written, NULL) &&
             written == u8.size();
        CloseHandle(hf);
    }
    MessageBoxW(h, ok ? (L"诊断报告已导出到：\n" + path).c_str()
                      : L"导出失败（无法写入文件，详见日志）",
                L"导出诊断报告", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    LX_LOG(L"ui", L"诊断报告导出 ok=%d path=%ls", (int)ok, path.c_str());
}

LRESULT CALLBACK StatsEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        DestroyWindow(GetParent(h)); // 焦点在正文时 Esc 关窗
        return 0;
    }
    return CallWindowProcW(g_oldStatsEditProc, h, msg, wp, lp);
}

LRESULT CALLBACK StatsWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE)
    {
        int cx = LOWORD(lp), cy = HIWORD(lp);
        int bodyH = cy > kStatsBarH ? cy - kStatsBarH : cy; // 正文让出底部按钮条
        if (g_hStatsEdit) MoveWindow(g_hStatsEdit, 0, 0, cx, bodyH, TRUE);
        int y = cy > kStatsBarH ? cy - kStatsBarH + 8 : 0;
        HWND b1 = GetDlgItem(h, IDC_STATS_REFRESH);
        HWND b2 = GetDlgItem(h, IDC_STATS_RESET);
        HWND b3 = GetDlgItem(h, IDC_STATS_EXPORT);
        if (b1) MoveWindow(b1, 12, y, 100, 32, TRUE);
        if (b2) MoveWindow(b2, 120, y, 100, 32, TRUE);
        if (b3) MoveWindow(b3, 228, y, 160, 32, TRUE);
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        switch (LOWORD(wp))
        {
        case IDC_STATS_REFRESH:
            StatsRefreshText();
            return 0;
        case IDC_STATS_RESET:
            if (MessageBoxW(h, L"确定清零全部打字统计？此操作不可恢复。",
                            L"清零统计", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            {
                StatsReset();
                StatsRefreshText();
                LX_LOG(L"ui", L"打字统计已清零");
            }
            return 0;
        case IDC_STATS_EXPORT:
            StatsExportReport(h);
            return 0;
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        DestroyWindow(h); // 统计窗本身持焦点时 Esc 关窗
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC) // 只读 EDIT 默认灰字 → 正常深色可读
    {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        SetTextColor(hdc, RGB(32, 32, 32));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    if (msg == WM_DESTROY)
    {
        g_hStats = nullptr;
        g_hStatsEdit = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void ShowUsageStatsWindow(HINSTANCE hInst)
{
    if (g_hStats && IsWindow(g_hStats))
    {
        ShowWindow(g_hStats, SW_RESTORE);
        SetForegroundWindow(g_hStats);
        StatsRefreshText();
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = StatsWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LingxiStats";
    RegisterClassW(&wc);

    const int W = 460, H = 400;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT wa{ 0, 0, 1920, 1040 };
    if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - H) / 2;

    g_hStats = CreateWindowExW(WS_EX_TOPMOST, L"LingxiStats", L"灵犀输入法 · 打字统计",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               x, y, W, H, NULL, NULL, hInst, NULL);
    if (!g_hStats) return;

    g_hStatsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                       ES_AUTOVSCROLL | WS_VSCROLL,
                                   0, 0, W, H - kStatsBarH, g_hStats, NULL, hInst, NULL);
    g_oldStatsEditProc = (WNDPROC)SetWindowLongPtrW(g_hStatsEdit, GWLP_WNDPROC,
                                                    (LONG_PTR)StatsEditProc);
    if (!g_statsFont)
        g_statsFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    SendMessageW(g_hStatsEdit, WM_SETFONT, (WPARAM)g_statsFont, TRUE);
    SetWindowTextW(g_hStatsEdit, BuildStatsText().c_str());
    SendMessageW(g_hStatsEdit, EM_SETSEL, 0, 0);
    // 底部按钮条：刷新 / 清零 / 导出诊断报告
    struct { int id; const wchar_t* text; int x, w; } btns[3] = {
        { IDC_STATS_REFRESH, L"刷新", 12, 100 },
        { IDC_STATS_RESET, L"清零...", 120, 100 },
        { IDC_STATS_EXPORT, L"导出诊断报告...", 228, 160 },
    };
    for (const auto& b : btns)
    {
        HWND hb = CreateWindowExW(0, L"BUTTON", b.text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  b.x, H - kStatsBarH + 8, b.w, 32, g_hStats,
                                  (HMENU)(INT_PTR)b.id, hInst, NULL);
        SendMessageW(hb, WM_SETFONT, (WPARAM)g_statsFont, TRUE);
    }
    UpdateWindow(g_hStats);
}

} // namespace lingxi
