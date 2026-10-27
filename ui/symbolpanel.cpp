#include "symbolpanel.h"
#include "../common/log.h"

#include <windowsx.h>
#include <cstdio>
#include <vector>
#include <string>

namespace lingxi {
namespace {

struct Category
{
    const wchar_t* name;
    const wchar_t* symbols;
};

const Category kCats[] = {
    { L"常用", L"，。、；：？！…—·「」『』《》【】（）" },
    { L"数学", L"±×÷≠≤≥≈∞∑∏√∂∫∈∧∨⊥∥∠⊙≡≌∆∇‰°′″" },
    { L"货币", L"￥＄€￠￡¥$¢£₩₽₹" },
    { L"箭头", L"←↑→↓↖↗↘↙⇐⇒⇑⇓⇔↔↕➜➔►◄▶▲▼◀◆◇○●◎☆★■□" },
    { L"表情", L"😀😁😂🤣😊😍😎🤔😴😭😡👍👏🙏🎉🔥⭐✨" },
};

constexpr int kCell = 38;
constexpr int kCols = 9;
constexpr int kCatH = 26;
constexpr int kPad = 4;

HWND g_hPanel = nullptr;
int g_cat = 0;
HFONT g_font = nullptr;
std::function<void(const std::wstring&)> g_onInsert;

const wchar_t* CurrentSymbols() { return kCats[g_cat].symbols; }

int SymCount() { return (int)wcslen(CurrentSymbols()); }

int PanelHeight()
{
    int rows = (SymCount() + kCols - 1) / kCols;
    return kCatH + kPad + rows * kCell + kPad;
}

int PanelWidth() { return kPad + kCell * kCols + kPad; }

void ClosePanel()
{
    if (g_hPanel) DestroyWindow(g_hPanel);
    g_hPanel = nullptr;
}

LRESULT CALLBACK PanelWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // 追踪关键生命周期消息
    if (msg == WM_DESTROY || msg == WM_CLOSE || msg == WM_NCDESTROY ||
        msg == WM_RBUTTONDOWN || msg == WM_KEYDOWN)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"tmp\\panel_msg.txt", L"a") == 0 && f)
        {
            fwprintf(f, L"msg=0x%X wp=%lu\n", msg, (unsigned long)wp);
            fclose(f);
        }
    }
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (y < kCatH)
        {
            int cw = (PanelWidth() - 2 * kPad) / 5;
            int c = (x - kPad) / cw;
            if (c >= 0 && c < 5 && c != g_cat)
            {
                g_cat = c;
                SetWindowPos(h, HWND_TOPMOST, 0, 0, PanelWidth(), PanelHeight(),
                             SWP_NOMOVE | SWP_NOACTIVATE);
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        int col = (x - kPad) / kCell;
        int row = (y - kCatH - kPad) / kCell;
        int idx = row * kCols + col;
        if (col >= 0 && col < kCols && idx >= 0 && idx < SymCount())
        {
            wchar_t s[2] = { CurrentSymbols()[idx], 0 };
            if (g_onInsert) g_onInsert(s);
            // 插入后保留面板（便于连续输入）；点击符号，不抢焦点
            return 0;
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        ClosePanel();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ClosePanel();
        return 0;
    case WM_ACTIVATEAPP:
        if (wp == FALSE) { /* 点击其他应用时不立即关闭，保持可用 */ }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(250, 250, 250));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(hdc, g_font);

        // 分类按钮
        int cw = (PanelWidth() - 2 * kPad) / 5;
        for (int i = 0; i < 5; i++)
        {
            RECT cr{ kPad + i * cw, 2, kPad + (i + 1) * cw, kCatH - 1 };
            if (i == g_cat)
            {
                HBRUSH hl = CreateSolidBrush(RGB(220, 236, 252));
                FillRect(hdc, &cr, hl);
                DeleteObject(hl);
                SetTextColor(hdc, RGB(20, 90, 180));
            }
            else
            {
                SetTextColor(hdc, RGB(60, 60, 60));
            }
            DrawTextW(hdc, kCats[i].name, -1, &cr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // 网格符号
        const wchar_t* sym = CurrentSymbols();
        SetTextColor(hdc, RGB(30, 30, 30));
        for (int i = 0; i < SymCount(); i++)
        {
            int c = i % kCols;
            int r = i / kCols;
            RECT cell{ kPad + c * kCell, kCatH + kPad + r * kCell,
                       kPad + (c + 1) * kCell, kCatH + kPad + (r + 1) * kCell };
            wchar_t s[2] = { sym[i], 0 };
            DrawTextW(hdc, s, -1, &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, oldFont);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_hPanel = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void ShowSymbolPanel(HINSTANCE hInst, std::function<void(const std::wstring&)> onInsert)
{
    g_onInsert = std::move(onInsert);
    if (g_hPanel)
    {
        SetWindowPos(g_hPanel, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LingxiSymbolPanel";
    ATOM atm = RegisterClassW(&wc);
    DWORD regErr = GetLastError();

    g_font = CreateFontW(20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    POINT pt;
    GetCursorPos(&pt);
    int x = pt.x;
    int y = pt.y;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (x + PanelWidth() > sw) x = sw - PanelWidth();
    if (y + PanelHeight() > sh) y = sh - PanelHeight();

    g_hPanel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"LingxiSymbolPanel", L"符号面板",
        WS_POPUP | WS_BORDER, x, y, PanelWidth(), PanelHeight(),
        0, 0, hInst, 0);
    DWORD createErr = GetLastError();

    FILE* f = nullptr;
    if (_wfopen_s(&f, L"tmp\\panel_diag.txt", L"a") == 0 && f)
    {
        fwprintf(f, L"atom=%u regErr=%lu font=%p hwnd=%p createErr=%lu\n",
                 atm, regErr, (void*)g_font, (void*)g_hPanel, createErr);
        fclose(f);
    }

    if (g_hPanel)
    {
        ShowWindow(g_hPanel, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hPanel);
    }
}

} // namespace lingxi
