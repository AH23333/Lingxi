#include "symbolpanel.h"
#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

#include <windowsx.h>
#include <string>

namespace lingxi {
namespace {

struct Category
{
    const wchar_t* name;          // 标签名（≤3 全角字符宽，kCatTabW 内放得下）
    const wchar_t* const* items;  // 条目：单符号或颜文字串（可含 VS16 变体选择符）
    int count;
    int cellW;                    // 单元格宽（颜文字加宽）
    int cols;                     // 列数
};

// ---------------- 分类内容（2026-10-29 扩充：对标 Win+. 的 emoji/颜文字深度） ----------------

const wchar_t* const kSymCommon[] = {
    L"，", L"。", L"、", L"；", L"：", L"？", L"！", L"…", L"—", L"·", L"〜", L"～",
    L"「", L"」", L"『", L"』", L"《", L"》", L"【", L"】", L"（", L"）", L"〔", L"〕",
    L"‘", L"’", L"“", L"”", L"〖", L"〗",
};

const wchar_t* const kSymMath[] = {
    L"±", L"×", L"÷", L"≠", L"≤", L"≥", L"≈", L"≡", L"≌", L"∞", L"∑", L"∏",
    L"√", L"∂", L"∫", L"∈", L"∉", L"⊆", L"⊇", L"∪", L"∩", L"∧", L"∨", L"⊥",
    L"∥", L"∠", L"∴", L"∵", L"‰", L"°", L"℉", L"℃", L"∆", L"∇",
};

const wchar_t* const kSymCurrency[] = {
    L"￥", L"＄", L"€", L"￠", L"￡", L"¥", L"$", L"¢", L"£", L"₩", L"₽", L"₹",
    L"₫", L"₴", L"₺", L"₦",
};

const wchar_t* const kSymArrow[] = {
    L"←", L"↑", L"→", L"↓", L"↖", L"↗", L"↘", L"↙", L"⇐", L"⇒", L"⇑", L"⇓",
    L"⇔", L"↔", L"↕", L"➜", L"➔", L"►", L"◄", L"▶", L"▼", L"◀", L"▲", L"▽",
    L"◈", L"◇", L"○", L"●", L"◎", L"☆", L"★", L"■", L"□",
};

const wchar_t* const kSymFace[] = {
    L"😀", L"😁", L"😂", L"🤣", L"😊", L"😋", L"😍", L"🥰", L"😘", L"😜", L"🤪", L"🤔",
    L"🤨", L"😐", L"😑", L"😶", L"🙄", L"😏", L"😣", L"😥", L"😭", L"😤", L"😠", L"😡",
    L"🥵", L"🥶", L"😴", L"🤤", L"🥴", L"🤢", L"🤮", L"🤯", L"🥳", L"😎", L"🤓",
};

const wchar_t* const kSymGesture[] = {
    L"👍", L"👎", L"👌", L"✌️", L"🤞", L"🤟", L"🤘", L"🤙", L"👈", L"👉", L"👆", L"👇",
    L"☝️", L"✋", L"🤚", L"🖐️", L"🖖", L"👋", L"🤝", L"🙏", L"💪", L"🦾", L"✊", L"👏",
    L"🤲",
};

const wchar_t* const kSymAnimal[] = {
    L"🐱", L"🐶", L"🐭", L"🐹", L"🐰", L"🦊", L"🐻", L"🐼", L"🐨", L"🐯", L"🦁", L"🐮",
    L"🐷", L"🐸", L"🐵", L"🐔", L"🐧", L"🐦", L"🦆", L"🦅", L"🦉", L"🐴", L"🦄", L"🐝",
    L"🦋", L"🐌", L"🐞", L"🐢", L"🐍", L"🐙", L"🦀", L"🐬", L"🐳", L"🐠",
};

const wchar_t* const kSymFood[] = {
    L"🍎", L"🍊", L"🍋", L"🍌", L"🍉", L"🍇", L"🍓", L"🍒", L"🍑", L"🥭", L"🍍", L"🥥",
    L"🥝", L"🍅", L"🥑", L"🍆", L"🥔", L"🥕", L"🌽", L"🌶️", L"🥦", L"🍄", L"🥜", L"🍞",
    L"🥐", L"🧀", L"🍗", L"🍔", L"🍟", L"🍕", L"🌭", L"🥪", L"🍜", L"🍣", L"🍩", L"🍪",
};

const wchar_t* const kSymActivity[] = {
    L"❤️", L"🧡", L"💛", L"💚", L"💙", L"💜", L"🖤", L"🤍", L"💕", L"💖", L"💗", L"💘",
    L"💝", L"🎉", L"🎊", L"🎈", L"🎁", L"🏆", L"⚽", L"🏀", L"🎮", L"🎲", L"🎵", L"🎶",
    L"📞", L"📧", L"💡", L"🔧", L"🔒", L"⏰",
};

const wchar_t* const kSymKaomoji[] = {
    L"(^_^)", L"(T_T)", L"(>_<)", L"(^o^)", L"(^3^)", L"(-_-)", L"(._.)", L"(¬_¬)",
    L"(＠_＠)", L"(⊙o⊙)", L"(⊙ω⊙)", L"(Ｔ▽Ｔ)", L"(￣▽￣)", L"(￣ー￣)", L"(ToT)/", L"/(^_^)",
    L"(>3<)", L"(;-;)", L"(X_X)", L"(O_O)", L"(o_o)", L"(Q_Q)", L"(^_~)", L"(^_-)",
};

const Category kCats[] = {
    { L"常用",   kSymCommon,   (int)(sizeof(kSymCommon) / sizeof(kSymCommon[0])),   38, 12 },
    { L"数学",   kSymMath,     (int)(sizeof(kSymMath) / sizeof(kSymMath[0])),      38, 12 },
    { L"货币",   kSymCurrency, (int)(sizeof(kSymCurrency) / sizeof(kSymCurrency[0])), 38, 12 },
    { L"箭头",   kSymArrow,    (int)(sizeof(kSymArrow) / sizeof(kSymArrow[0])),    38, 12 },
    { L"表情",   kSymFace,     (int)(sizeof(kSymFace) / sizeof(kSymFace[0])),      38, 12 },
    { L"手势",   kSymGesture,  (int)(sizeof(kSymGesture) / sizeof(kSymGesture[0])), 38, 12 },
    { L"动物",   kSymAnimal,   (int)(sizeof(kSymAnimal) / sizeof(kSymAnimal[0])),   38, 12 },
    { L"食物",   kSymFood,     (int)(sizeof(kSymFood) / sizeof(kSymFood[0])),       38, 12 },
    { L"活动",   kSymActivity, (int)(sizeof(kSymActivity) / sizeof(kSymActivity[0])), 38, 12 },
    { L"^_^",   kSymKaomoji,  (int)(sizeof(kSymKaomoji) / sizeof(kSymKaomoji[0])), 104, 4 },
};

constexpr int kCell = 38;      // 单元格高（宽随分类：普通 38 / 颜文字 104）
constexpr int kCatH = 26;
constexpr int kPad = 4;
constexpr int kCatTabW = 48;   // 标签宽（10 分类×48=480，3 全角字标签也放得下）
constexpr int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));

HWND g_hPanel = nullptr;
int g_cat = 0;
HFONT g_font = nullptr;
std::function<void(const std::wstring&)> g_onInsert;

const Category& CurCat() { return kCats[g_cat]; }

int PanelHeight()
{
    const Category& c = CurCat();
    int rows = (c.count + c.cols - 1) / c.cols;
    return kCatH + kPad + rows * kCell + kPad;
}

int PanelWidth() { return kPad + kCatTabW * kCatCount + kPad; }

// 网格水平居中于面板（不同分类网格宽不同）
int GridX() { return kPad + (PanelWidth() - 2 * kPad - CurCat().cellW * CurCat().cols) / 2; }

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
            int c = (x - kPad) / kCatTabW;
            if (c >= 0 && c < kCatCount && c != g_cat)
            {
                g_cat = c;
                SetWindowPos(h, HWND_TOPMOST, 0, 0, PanelWidth(), PanelHeight(),
                             SWP_NOMOVE | SWP_NOACTIVATE);
                UiApplyRoundCorners(h, PanelWidth(), PanelHeight()); // RGN 路径随尺寸重设
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        const Category& cat = CurCat();
        int gx = GridX();
        int col = (x - gx) / cat.cellW;
        int row = (y - kCatH - kPad) / kCell;
        int idx = row * cat.cols + col;
        if (col >= 0 && col < cat.cols && x >= gx && idx >= 0 && idx < cat.count)
        {
            if (g_onInsert) g_onInsert(std::wstring(cat.items[idx]));
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
        const SkinDef& skin = UiSkin(LoadSettings().skinIndex);

        // WinUI 浮层：白底 + 8px 圆角浅描边（RGN/DWM 负责裁圆角）
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(hdc, &rc, white);
        {
            HPEN pen = CreatePen(PS_SOLID, 1, kUiFlyoutBorder);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 16, 16);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(hdc, g_font);

        // 分类按钮：选中=皮肤浅底药丸 + 强调色字；未选中=次级灰字
        for (int i = 0; i < kCatCount; i++)
        {
            RECT cr{ kPad + i * kCatTabW, 2, kPad + (i + 1) * kCatTabW, kCatH - 1 };
            if (i == g_cat)
            {
                RECT pr = cr;
                InflateRect(&pr, -2, -1);
                HBRUSH hl = CreateSolidBrush(skin.hoverLight);
                HGDIOBJ oldBrush = SelectObject(hdc, hl);
                HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
                int pill = (pr.bottom - pr.top) * 2; // 全高药丸圆角
                RoundRect(hdc, pr.left, pr.top, pr.right + 1, pr.bottom + 1, pill, pill);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
                DeleteObject(hl);
                SetTextColor(hdc, skin.accent);
            }
            else
            {
                SetTextColor(hdc, kUiText2);
            }
            DrawTextW(hdc, kCats[i].name, -1, &cr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // 网格符号（字符串条目：单符号/颜文字/含 VS16 变体选择符的 emoji）
        const Category& cat = CurCat();
        int gx = GridX();
        SetTextColor(hdc, kUiText);
        for (int i = 0; i < cat.count; i++)
        {
            int c = i % cat.cols;
            int r = i / cat.cols;
            RECT cell{ gx + c * cat.cellW, kCatH + kPad + r * kCell,
                       gx + (c + 1) * cat.cellW, kCatH + kPad + (r + 1) * kCell };
            DrawTextW(hdc, cat.items[i], -1, &cell,
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
    wc.style = CS_DROPSHADOW; // WinUI 浮层级浅投影
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
        WS_POPUP, x, y, PanelWidth(), PanelHeight(),
        0, 0, hInst, 0);
    if (g_hPanel)
    {
        UiApplyRoundCorners(g_hPanel, PanelWidth(), PanelHeight()); // 圆角：描边自绘
        ShowWindow(g_hPanel, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hPanel);
    }
}

} // namespace lingxi
