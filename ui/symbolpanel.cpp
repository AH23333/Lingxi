#include "symbolpanel.h"
#include "../common/log.h"

#include <windowsx.h>
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
constexpr int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));

HWND g_hPanel = nullptr;
int g_cat = 0;
HFONT g_font = nullptr;
std::function<void(const std::wstring&)> g_onInsert;

const wchar_t* CurrentSymbols() { return kCats[g_cat].symbols; }

// 码点数量：UTF-16 代理对按 1 个码点计。
int SymCount(const wchar_t* s)
{
    int n = 0;
    for (int i = 0; s[i];)
    {
        if (IS_SURROGATE_PAIR(s[i], s[i + 1])) i += 2;
        else i += 1;
        ++n;
    }
    return n;
}

// 第 idx 个码点的 wchar 偏移与长度（代理对 len=2）。
void SymAt(const wchar_t* s, int idx, int* off, int* len)
{
    int o = 0;
    for (int i = 0; i < idx; ++i)
        o += (IS_SURROGATE_PAIR(s[o], s[o + 1])) ? 2 : 1;
    *off = o;
    *len = (IS_SURROGATE_PAIR(s[o], s[o + 1])) ? 2 : 1;
}

int PanelHeight()
{
    int rows = (SymCount(CurrentSymbols()) + kCols - 1) / kCols;
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
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (y < kCatH)
        {
            int cw = (PanelWidth() - 2 * kPad) / kCatCount;
            int c = (x - kPad) / cw;
            if (c >= 0 && c < kCatCount && c != g_cat)
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
        int total = SymCount(CurrentSymbols());
        if (col >= 0 && col < kCols && idx >= 0 && idx < total)
        {
            int off = 0, len = 1;
            SymAt(CurrentSymbols(), idx, &off, &len);
            std::wstring s(CurrentSymbols() + off, len);
            if (g_onInsert) g_onInsert(s);
            // 插入后保留面板（便于连续输入）；点击符号不抢焦点
            return 0;
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        ClosePanel();
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
        int cw = (PanelWidth() - 2 * kPad) / kCatCount;
        for (int i = 0; i < kCatCount; i++)
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

        // 网格符号（按码点步进，cell 与点击换算一致）
        const wchar_t* sym = CurrentSymbols();
        int total = SymCount(sym);
        SetTextColor(hdc, RGB(30, 30, 30));
        for (int i = 0; i < total; i++)
        {
            int c = i % kCols;
            int r = i / kCols;
            RECT cell{ kPad + c * kCell, kCatH + kPad + r * kCell,
                       kPad + (c + 1) * kCell, kCatH + kPad + (r + 1) * kCell };
            int off = 0, len = 1;
            SymAt(sym, i, &off, &len);
            DrawTextW(hdc, sym + off, len, &cell,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    RegisterClassW(&wc);

    // 字体进程内复用：仅首次创建，避免每次开面板泄漏一个 HFONT。
    if (!g_font)
        g_font = CreateFontW(20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    // 按鼠标所在显示器（含副屏负坐标）的工作区钳位
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT w = mi.rcWork;
    int x = pt.x;
    int y = pt.y;
    if (x + PanelWidth() > w.right) x = w.right - PanelWidth();
    if (x < w.left) x = w.left;
    if (y + PanelHeight() > w.bottom) y = w.bottom - PanelHeight();
    if (y < w.top) y = w.top;

    g_hPanel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"LingxiSymbolPanel", L"符号面板",
        WS_POPUP | WS_BORDER, x, y, PanelWidth(), PanelHeight(),
        0, 0, hInst, 0);
    if (g_hPanel)
    {
        ShowWindow(g_hPanel, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hPanel);
    }
}

} // namespace lingxi
