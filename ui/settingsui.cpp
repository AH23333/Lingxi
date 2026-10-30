//
// 设置窗口（Win11 设置风格：左导航页 → 白卡 → 行）。
// 原 2582 行聚合文件按 P1 重构拆分后，本文件只保留设置窗本体；
// 托盘/词典窗/帮助窗/统计窗/备份分别见 tray.cpp / userdictwnd.cpp /
// helpwnd.cpp / statswnd.cpp / settingsbackup.cpp，跨文件共享面在
// uiinternal.h。
//
// 设置存 HKCU\Software\LingxiIme（common/settings），DLL 与 UI 每次读取实时生效：
// - 拼音组合模式：DLL 在模式检测（焦点/激活/语言变化）时读取
// - 候选数量 / 界面明暗 / 英文逐键补全：UI 在每次预测请求时读取
//
#include "settingsui.h"
#include "uiinternal.h"
#include "hotphraseui.h"
#include "phraseui.h"

#include <windows.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
// GDI+：抗锯齿圆角/圆绘制（GDI RoundRect/Ellipse 无 AA，药丸/卡片边缘锯齿）
#define NOMINMAX
#include <gdiplus.h>
#include <algorithm>
#pragma comment(lib, "gdiplus.lib")

#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kSettingsWndClass[] = L"LingxiSettingsWnd";

// ---------------- GDI+ 抗锯齿绘制助手 ----------------
// GDI RoundRect/Ellipse 无抗锯齿，真机肉眼可见锯齿；GDI+ GraphicsPath+
// SmoothingModeAntiAlias 根治。令牌进程生命周期持有（UI 进程退出由系统回收）。

ULONG_PTR g_gdipToken = 0;

void EnsureGdip()
{
    if (g_gdipToken) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdipToken, &in, NULL);
}

Gdiplus::Color GdipColor(COLORREF c)
{
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 填充圆角矩形（可选描边）。radius=高度一半即药丸形。
void GdipRoundRect(HDC hdc, RECT rc, COLORREF fill, int radius,
                   COLORREF border = 0, int borderW = 0)
{
    EnsureGdip();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    int d = radius * 2;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    d = (std::min)(d, (std::min)(w, h));
    Gdiplus::Rect r(rc.left, rc.top, w, h);
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + w - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + w - d, r.Y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::SolidBrush br(GdipColor(fill));
    g.FillPath(&br, &path);
    if (borderW > 0)
    {
        Gdiplus::Pen pen(GdipColor(border), (Gdiplus::REAL)borderW);
        pen.SetAlignment(Gdiplus::PenAlignmentInset); // 描边不溢出控件矩形
        g.DrawPath(&pen, &path);
    }
}

void GdipFillEllipse(HDC hdc, RECT rc, COLORREF fill)
{
    EnsureGdip();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(GdipColor(fill));
    g.FillEllipse(&br, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

constexpr int IDC_CHK_PINYIN = 101;
constexpr int IDC_CHK_EN = 102;
constexpr int IDC_EDT_COUNT = 104;
constexpr int IDC_CMB_FONTSIZE = 105;
constexpr int IDC_CMB_DARK = 106;
constexpr int IDC_CHK_CNPUNCT = 107;
constexpr int IDC_CHK_FUZZY = 108;
// 模糊音组细分（bit0..bit4 对应 Settings.fuzzyGroups）
constexpr int IDC_CHK_FZ_ZH = 121;   // zh/z
constexpr int IDC_CHK_FZ_CH = 122;   // ch/c
constexpr int IDC_CHK_FZ_SH = 123;   // sh/s
constexpr int IDC_CHK_FZ_NL = 124;   // n/l
constexpr int IDC_CHK_FZ_NG = 125;   // 前后鼻音
constexpr int IDC_CHK_FZ_FH = 127;   // f/h
constexpr int IDC_CHK_FZ_LR = 128;   // l/r
constexpr int IDC_CHK_FZ_GK = 129;   // g/k
constexpr int IDC_CMB_TOGGLEMODE = 117; // B5 草案②：中英切换键（旧 109 复选框弃用）
constexpr int IDC_BTN_USERDICT = 111;
constexpr int IDC_CHK_SHUANGPIN = 112;
constexpr int IDC_CHK_SYMBOLHOTKEY = 113;
constexpr int IDC_CHK_TRADITIONAL = 114;
constexpr int IDC_CMB_SKIN = 115;
constexpr int IDC_CHK_LLMRERANK = 116;
constexpr int IDC_CHK_LLMMULTIPATH = 119; // LLM 多路采样（R3 §3，默认关）
constexpr int IDC_BTN_HOTPHRASES = 118; // 快捷短语管理入口
constexpr int IDC_BTN_PHRASES = 120;    // R2 自定义短语管理入口
constexpr int IDC_CMB_OPACITY = 126;    // 候选窗不透明度
constexpr int IDC_CHK_ASSOCPICK = 130;  // 选词联想开关（GAP §11)
constexpr int IDC_CMB_UIFONT = 131;     // 界面字体族（候选窗/浮层共用）
constexpr int IDC_BTN_STATS = 132;      // 打字统计入口（词典页"词典与工具"卡）

HWND g_hSettings = nullptr; // 设置窗口（关闭=隐藏，常驻）
HWND g_hChkPinyin = nullptr, g_hChkEn = nullptr, g_hEdtCount = nullptr,
     g_hCmbDark = nullptr, g_hCmbFontSize = nullptr, g_hCmbSkin = nullptr,
     g_hCmbToggleMode = nullptr,
     g_hChkCnPunct = nullptr, g_hChkFuzzy = nullptr,
     g_hChkShuangpin = nullptr, g_hChkSymbolHotkey = nullptr, g_hChkTraditional = nullptr,
      g_hChkLlmRerank = nullptr, g_hChkLlmMultiPath = nullptr, g_hChkAssocPick = nullptr;
HWND g_hChkFz[8] = {}; // 模糊音组细分开关（8 组）
HWND g_hCmbOpacity = nullptr;
HWND g_hCmbUiFont = nullptr; // 界面字体族下拉
HFONT g_hFontLabel = nullptr;   // 设置行标题 14px
HFONT g_hFontDesc = nullptr;    // 设置行描述 12px（次级色）
HFONT g_hFontTitle = nullptr;   // 设置页标题 20px 半粗
HFONT g_hFontNav = nullptr;     // 设置导航标题 15px 半粗
HFONT g_hFontCardHdr = nullptr; // 设置卡片标题 14px 半粗
bool g_updating = false; // 程序性改控件时不回写设置
bool g_controlsLoaded = false; // 控件尚未从设置加载同步——禁止回写（防刷零事故）
// BS_AUTOCHECKBOX|BS_OWNERDRAW 位或后=0xB=纯 BS_OWNERDRAW：自绘按钮不维护勾选
// 态，BM_GETCHECK 恒返 0——历史两次"整页刷零"事故与本次设置全零的真凶。
// 开关状态改由此映射外置维护，绘制/读写/点击翻转全走它。
std::unordered_map<HWND, bool> g_toggleOn;
void SetToggle(HWND h, bool on)
{
    if (!h) return;
    g_toggleOn[h] = on;
    InvalidateRect(h, NULL, TRUE); // owner-draw 无自动重绘：翻转必须手动失效
}
bool ToggleOn(HWND h)
{
    auto it = g_toggleOn.find(h);
    return it != g_toggleOn.end() && it->second;
}

// ---------------- 设置读写 ----------------

void LoadToControls()
{
    Settings s = LoadSettings();
    g_updating = true;
    SetToggle(g_hChkPinyin, s.pinyinEnabled);
    SetToggle(g_hChkEn, s.enCompletion);
    SetToggle(g_hChkCnPunct, s.cnPunct);
    SetToggle(g_hChkFuzzy, s.fuzzyPinyin);
    for (int fz = 0; fz < 8; fz++) SetToggle(g_hChkFz[fz], (s.fuzzyGroups >> fz) & 1u);
    ComboBox_SetCurSel(g_hCmbToggleMode, (int)(s.toggleMode <= 4 ? s.toggleMode : 1));
    SetToggle(g_hChkShuangpin, s.shuangpin);
    SetToggle(g_hChkSymbolHotkey, s.symbolHotkey);
    SetToggle(g_hChkTraditional, s.traditionalOutput);
    SetToggle(g_hChkLlmRerank, s.llmRerank);
    { const uint32_t kOps[] = {100, 90, 80, 70}; int oi = 0;
      for (int z = 0; z < 4; z++) if (s.candidateOpacity == kOps[z]) oi = z;
      if (g_hCmbOpacity) ComboBox_SetCurSel(g_hCmbOpacity, oi); }
    SetToggle(g_hChkLlmMultiPath, s.llmMultiPath);
    SetToggle(g_hChkAssocPick, s.assocAfterPick);
    wchar_t buf[16];
    _snwprintf_s(buf, _TRUNCATE, L"%u", s.candidateCount);
    Edit_SetText(g_hEdtCount, buf);
    static const uint32_t kFontSizes[] = {12, 14, 16, 18, 20, 24, 28, 32};
    int fsSel = 2;
    for (int i = 0; i < 8; i++)
        if (kFontSizes[i] == s.candidateFontSize) fsSel = i;
    ComboBox_SetCurSel(g_hCmbFontSize, fsSel);
    ComboBox_SetCurSel(g_hCmbSkin, (int)s.skinIndex);
    ComboBox_SetCurSel(g_hCmbDark, (int)s.darkMode);
    ComboBox_SetCurSel(g_hCmbUiFont, (int)(s.uiFont <= 3 ? s.uiFont : 0));
    g_controlsLoaded = true;
    g_updating = false;
}

void SaveFromControls()
{
    // 双保险（2026-10-29 复盘：真实设置两次被"整页回写未同步控件"刷零）：
    // ① 控件从未 LoadToControls 过 → 拒写；② 改用 diff 落盘，未变字段不触碰。
    if (g_updating || !g_controlsLoaded) return;
    Settings s = LoadSettings(); // 保留未在此窗口编辑的字段
    s.pinyinEnabled = ToggleOn(g_hChkPinyin);
    s.enCompletion = ToggleOn(g_hChkEn);
    s.cnPunct = ToggleOn(g_hChkCnPunct);
    s.fuzzyPinyin = ToggleOn(g_hChkFuzzy);
    { unsigned grp = 0; for (int fz = 0; fz < 8; fz++) if (ToggleOn(g_hChkFz[fz])) grp |= 1u << fz;
      s.fuzzyGroups = grp ? grp : 0xFF; } // 一组未勾=退回全组（总开关才是关闭入口，避免"全关静默失聪"）
    int tsel = ComboBox_GetCurSel(g_hCmbToggleMode);
        if (tsel >= 0 && tsel <= 4) s.toggleMode = (uint32_t)tsel;
    s.shuangpin = ToggleOn(g_hChkShuangpin);
    s.symbolHotkey = ToggleOn(g_hChkSymbolHotkey);
    s.traditionalOutput = ToggleOn(g_hChkTraditional);
    s.llmRerank = ToggleOn(g_hChkLlmRerank);
    { const uint32_t kOps[] = {100, 90, 80, 70}; int oi = g_hCmbOpacity ? ComboBox_GetCurSel(g_hCmbOpacity) : 0;
      s.candidateOpacity = kOps[oi >= 0 && oi < 4 ? oi : 0]; }
    s.llmMultiPath = ToggleOn(g_hChkLlmMultiPath);
    s.assocAfterPick = ToggleOn(g_hChkAssocPick);
    wchar_t buf[16] = L"";
    Edit_GetText(g_hEdtCount, buf, 16);
    int n = _wtoi(buf);
    if (n < 1) n = 1;
    if (n > 9) n = 9;
    s.candidateCount = (uint32_t)n;
    static const uint32_t kFontSizesSave[] = {12, 14, 16, 18, 20, 24, 28, 32};
    int fsIdx = ComboBox_GetCurSel(g_hCmbFontSize);
    if (fsIdx >= 0 && fsIdx < 8) s.candidateFontSize = kFontSizesSave[fsIdx];
    int skinSel = ComboBox_GetCurSel(g_hCmbSkin);
    if (skinSel >= 0 && skinSel <= 6) s.skinIndex = (uint32_t)skinSel;
    int fontSel = ComboBox_GetCurSel(g_hCmbUiFont);
    if (fontSel >= 0 && fontSel <= 3) s.uiFont = (uint32_t)fontSel;
    int sel = ComboBox_GetCurSel(g_hCmbDark);
    if (sel >= 0) s.darkMode = (uint32_t)sel;
    SaveSettingsDiff(s);
    if (g_hooks.settingsChanged) g_hooks.settingsChanged();
    if (g_hSettings) InvalidateRect(g_hSettings, NULL, TRUE); // 换肤等即时重绘自绘控件
    LX_LOG(L"ui", L"设置已保存: pinyin=%d count=%u dark=%u en=%d cnPunct=%d fuzzy=%d toggleMode=%u",
           (int)s.pinyinEnabled, s.candidateCount, s.darkMode, (int)s.enCompletion,
           (int)s.cnPunct, (int)s.fuzzyPinyin, (unsigned)s.toggleMode);
}

// ---------------- Win11 设置结构模型：页 → 卡 → 行 ----------------
// 控件一次创建、按页显隐；LayoutSettings() 只排当前页，PaintSettings()
// 绘制导航/卡面/分隔线/底部提示。

// 右侧控件形态
enum CtlKind
{
    CK_TOGGLE, // Win11 药丸开关（BS_OWNERDRAW 自绘，状态外置 g_toggleOn）
    CK_COMBO,  // CBS_DROPDOWNLIST 下拉
    CK_EDIT,   // 数字输入
    CK_BUTTON, // 强调色按钮（词典/短语入口）
    CK_NONE,   // 纯文本行（无控件）
};

struct SetRow
{
    HWND lab = nullptr;   // 行标题
    HWND desc = nullptr;  // 行描述（可空）
    HWND ctl = nullptr;   // 右侧控件（可空）
    CtlKind kind = CK_NONE;
    int ctlW = 0;         // 控件宽（96dpi 基准）
};

struct SetCard
{
    const wchar_t* header = nullptr; // 卡片标题（PaintSettings 绘制，可空）
    std::vector<SetRow> rows;
    RECT rc{};             // 排布后的卡面（客户区坐标，含滚动偏移）
    std::vector<int> divY; // 行分隔线 y（客户区坐标）
};

struct SetPage
{
    const wchar_t* nav = nullptr; // 导航项文本 = 页标题
    std::vector<SetCard> cards;
};

std::vector<SetPage> g_pages;
std::vector<HWND> g_descWnds;    // 描述文本框（CTLCOLOR 用次级色）
int g_curPage = 0;               // 当前页索引
int g_navHover = -1;             // 导航 hover 项（-1 无）
bool g_trackingMouse = false;    // TrackMouseEvent 已挂
int g_titleY = 0;                // 页标题 y（PaintSettings 用）
std::vector<HWND> g_accentBtns;  // 强调色按钮（WM_DRAWITEM 分发用）
int g_setMinW = 0, g_setMinH = 0; // WM_GETMINMAXINFO 下限（DPI 缩放后）
int g_setDpi = 96;
int g_scrollY = 0; // 设置页纵向滚动偏移（内容超出客户区时）
constexpr int kNavW = 216; // 96dpi 基准导航宽

int Scaled(int v) { return MulDiv(v, g_setDpi, 96); }

// 行高：带描述 60、纯标题 44（96dpi 基准）
int RowHeight(const SetRow& r) { return Scaled(r.desc ? 60 : 44); }

// 当前页行控件按 g_curPage 显隐（页面切换调用）
void ApplyPageVisibility()
{
    for (size_t p = 0; p < g_pages.size(); p++)
        for (auto& card : g_pages[p].cards)
            for (auto& row : card.rows)
            {
                int show = p == (size_t)g_curPage ? SW_SHOW : SW_HIDE;
                if (row.lab) ShowWindow(row.lab, show);
                if (row.desc) ShowWindow(row.desc, show);
                if (row.ctl) ShowWindow(row.ctl, show);
            }
}

// 排布当前页：页标题 → 逐卡（标题区/行/分隔线）——标签/描述左侧，控件右
// 缘对齐垂直居中。返回虚拟内容总高（滚动范围用）。
int LayoutSettings()
{
    if (!g_hSettings || g_pages.empty()) return 0;
    RECT rc{};
    GetClientRect(g_hSettings, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return 0;
    int navW = Scaled(kNavW);
    int pad = Scaled(24);     // 内容区外边距
    int cardPad = Scaled(16); // 卡片内边距
    int gap = Scaled(12);     // 卡片间距
    int x0 = navW + pad;
    int cardW = cw - x0 - pad;
    if (cardW < Scaled(240)) cardW = Scaled(240);

    g_titleY = Scaled(20) - g_scrollY;
    int y = g_titleY + Scaled(40) + Scaled(8);
    for (auto& card : g_pages[g_curPage].cards)
    {
        int top = y;
        card.divY.clear();
        if (card.header) // 卡片标题区 + 标题下分隔线
        {
            y += Scaled(38);
            card.divY.push_back(y);
        }
        else
        {
            y += Scaled(12);
        }
        bool first = true;
        for (auto& row : card.rows)
        {
            if (!first && row.lab) card.divY.push_back(y); // 行间分隔线
            int rh = RowHeight(row);
            int textW = cardW - cardPad * 2 - (row.ctl ? Scaled(row.ctlW) + Scaled(24) : 0);
            if (row.lab)
                MoveWindow(row.lab, x0 + cardPad, y + (row.desc ? Scaled(8) : Scaled(11)),
                           textW, Scaled(22), TRUE);
            if (row.desc)
                MoveWindow(row.desc, x0 + cardPad, y + Scaled(33), textW, Scaled(18), TRUE);
            if (row.ctl)
            {
                int ctlH = 0;
                switch (row.kind)
                {
                case CK_TOGGLE: ctlH = Scaled(24); break;
                case CK_COMBO: ctlH = Scaled(30); break;
                case CK_EDIT: ctlH = Scaled(30); break;
                case CK_BUTTON: ctlH = Scaled(32); break;
                default: break;
                }
                MoveWindow(row.ctl, x0 + cardW - cardPad - Scaled(row.ctlW),
                           y + (rh - ctlH) / 2, Scaled(row.ctlW), ctlH, TRUE);
            }
            y += rh;
            first = false;
        }
        y += Scaled(12); // 卡片底内边距
        card.rc = { x0, top, x0 + cardW, y };
        y += gap;
    }
    return y - gap + g_scrollY + Scaled(56); // 底部提示保护区
}

// Win11 观感统一绘制：中性底 + 导航（选中药丸+左缘强调条）+ 页标题 + 白卡
// 圆角描边 + 行分隔线 + 底部提示。子控件（标签/开关/下拉）由系统随后自绘。
void PaintSettings(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0 || g_pages.empty() || !g_hFontNav)
    {
        EndPaint(hwnd, &ps);
        return;
    }

    HBRUSH bg = CreateSolidBrush(kUiBg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    int navW = Scaled(kNavW);
    COLORREF accent = UiSkin(LoadSettings().skinIndex).accent;
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(hdc, g_hFontNav);

    // 导航标题
    SetTextColor(hdc, kUiText);
    RECT ntr{ Scaled(18), Scaled(14), navW, Scaled(44) };
    DrawTextW(hdc, L"灵犀输入法", -1, &ntr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 导航项：选中/热态浅灰药丸 + 选中左缘 3px 强调条
    SelectObject(hdc, g_hFontLabel);
    int itemH = Scaled(36), navTop = Scaled(56);
    for (int i = 0; i < (int)g_pages.size(); i++)
    {
        RECT item{ Scaled(10), navTop + i * itemH, navW - Scaled(10),
                   navTop + (i + 1) * itemH - Scaled(2) };
        bool sel = i == g_curPage;
        if (sel || i == g_navHover)
        {
            GdipRoundRect(hdc, item, sel ? RGB(229, 229, 229) : RGB(238, 238, 238), 8);
        }
        if (sel)
        {
            HBRUSH ib = CreateSolidBrush(accent);
            RECT bar{ item.left + Scaled(6), item.top + Scaled(8),
                      item.left + Scaled(9), item.bottom - Scaled(8) };
            FillRect(hdc, &bar, ib);
            DeleteObject(ib);
        }
        RECT ttr{ item.left + Scaled(18), item.top, item.right - Scaled(4), item.bottom };
        DrawTextW(hdc, g_pages[i].nav, -1, &ttr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 页标题
    int x0 = navW + Scaled(24);
    SelectObject(hdc, g_hFontTitle);
    RECT ptr{ x0, g_titleY, cw - Scaled(8), g_titleY + Scaled(38) };
    DrawTextW(hdc, g_pages[g_curPage].nav, -1, &ptr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 白卡（8px 圆角 + 1px 浅描边，GDI+ 抗锯齿）
    int rr = Scaled(16);
    for (auto& card : g_pages[g_curPage].cards)
        GdipRoundRect(hdc, card.rc, kUiCard, rr, kUiCardBorder, 1);

    // 卡片标题 + 行分隔线
    HPEN lpen = CreatePen(PS_SOLID, 1, RGB(238, 238, 238));
    HGDIOBJ lp = SelectObject(hdc, lpen);
    for (auto& card : g_pages[g_curPage].cards)
    {
        if (card.header)
        {
            SelectObject(hdc, g_hFontCardHdr);
            RECT htr{ card.rc.left + Scaled(16), card.rc.top + Scaled(8),
                      card.rc.right - Scaled(8), card.rc.top + Scaled(38) };
            DrawTextW(hdc, card.header, -1, &htr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        for (int dy : card.divY)
        {
            MoveToEx(hdc, card.rc.left + Scaled(16), dy, NULL);
            LineTo(hdc, card.rc.right - Scaled(16), dy);
        }
    }
    SelectObject(hdc, lp);
    DeleteObject(lpen);

    // 底部提示（内容区左下，不随滚动）
    SelectObject(hdc, g_hFontDesc);
    SetTextColor(hdc, kUiText2);
    RECT ftr{ x0, ch - Scaled(34), cw - Scaled(16), ch - Scaled(10) };
    DrawTextW(hdc, L"修改即时生效并自动保存 · 重建输入法后需重启应用加载新版",
              -1, &ftr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
}

// 滚动条开关+范围同步：内容超出客户区才显示 WS_VSCROLL（FRAMECHANGED 会
// 再触发一次 WM_SIZE→LayoutSettings，样式已一致，收敛）
void UpdateSettingsScrollbar(int contentH)
{
    if (!g_hSettings) return;
    RECT rc{};
    GetClientRect(g_hSettings, &rc);
    int clientH = rc.bottom;
    bool needScroll = contentH > clientH;
    LONG_PTR style = GetWindowLongPtrW(g_hSettings, GWL_STYLE);
    bool hasScroll = (style & WS_VSCROLL) != 0;
    if (needScroll != hasScroll)
    {
        SetWindowLongPtrW(g_hSettings, GWL_STYLE,
                          needScroll ? (style | WS_VSCROLL) : (style & ~WS_VSCROLL));
        SetWindowPos(g_hSettings, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    if (!needScroll)
    {
        if (g_scrollY != 0)
        {
            g_scrollY = 0;
            LayoutSettings();
            InvalidateRect(g_hSettings, NULL, TRUE); // 卡片随内容回移重绘
        }
        return;
    }
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = contentH;
    si.nPage = clientH > 0 ? (UINT)clientH : 1;
    int maxPos = contentH - clientH;
    if (g_scrollY > maxPos) g_scrollY = maxPos;
    if (g_scrollY < 0) g_scrollY = 0;
    si.nPos = g_scrollY;
    SetScrollInfo(g_hSettings, SB_VERT, &si, TRUE);
}

// ---- Win11 风格 owner-draw：药丸开关 / 强调色按钮 ----

// Win11 开关（ToggleSwitch）：44×24 热区内绘 40×20 全圆角药丸——
// 开=皮肤强调色+白钮居右；关=浅灰底+深灰钮居左+1px 灰描边。
// 纯 BS_OWNERDRAW：状态读外置 g_toggleOn（勾选位会被 OWNERDRAW 归并吞掉）。
void DrawToggle(const DRAWITEMSTRUCT* dis)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    // 控件背景补白（BUTTON 默认 BTNFACE 擦除会露灰，须盖成卡片白）
    HBRUSH hCard = CreateSolidBrush(kUiCard);
    FillRect(hdc, &rc, hCard);
    DeleteObject(hCard);

    int pillW = MulDiv(40, g_setDpi, 96), pillH = MulDiv(20, g_setDpi, 96);
    int px = rc.left + ((rc.right - rc.left) - pillW) / 2;
    int py = rc.top + ((rc.bottom - rc.top) - pillH) / 2;
    RECT pr{ px, py, px + pillW, py + pillH };
    bool on = ToggleOn(dis->hwndItem); // 外置状态（BS_OWNERDRAW 无勾选态）
    COLORREF accent = UiSkin(LoadSettings().skinIndex).accent;

    // GDI+ 抗锯齿药丸（GDI RoundRect 无 AA，真机边缘锯齿）
    if (on)
        GdipRoundRect(hdc, pr, accent, pillH / 2);
    else // 关：1px 灰描边（浅灰底在白卡上要有可见边界）
        GdipRoundRect(hdc, pr, RGB(234, 234, 234), pillH / 2, kUiControlEdge, 1);

    // 滑钮：12px 圆（开=白居右；关=深灰居左），GDI+ 抗锯齿
    int knob = MulDiv(12, g_setDpi, 96);
    int kx = on ? pr.right - knob - MulDiv(4, g_setDpi, 96)
                : pr.left + MulDiv(4, g_setDpi, 96);
    int ky = pr.top + (pillH - knob) / 2;
    GdipFillEllipse(hdc, RECT{ kx, ky, kx + knob, ky + knob },
                    on ? RGB(255, 255, 255) : RGB(90, 90, 90));
}

// 强调色按钮：皮肤色圆角底 + 白字（按下加深 25%）
void DrawAccentBtn(const DRAWITEMSTRUCT* dis)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF accent = UiSkin(LoadSettings().skinIndex).accent;
    if (pressed)
        accent = RGB(GetRValue(accent) * 3 / 4, GetGValue(accent) * 3 / 4,
                     GetBValue(accent) * 3 / 4);
    GdipRoundRect(hdc, rc, accent, 4);

    wchar_t txt[128] = {};
    GetWindowTextW(dis->hwndItem, txt, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(hdc, g_hFontLabel);
    DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CHK_PINYIN:
        case IDC_CHK_EN:
        case IDC_CHK_CNPUNCT:
        case IDC_CHK_FUZZY:
        case IDC_CHK_FZ_ZH: case IDC_CHK_FZ_CH: case IDC_CHK_FZ_SH:
        case IDC_CHK_FZ_NL: case IDC_CHK_FZ_NG:
        case IDC_CHK_FZ_FH: case IDC_CHK_FZ_LR: case IDC_CHK_FZ_GK:
        case IDC_CHK_SHUANGPIN:
        case IDC_CHK_SYMBOLHOTKEY:
        case IDC_CHK_TRADITIONAL:
        case IDC_CHK_LLMRERANK:
        case IDC_CMB_OPACITY:
        case IDC_CHK_LLMMULTIPATH:
            // owner-draw 无自动勾选：先翻外置状态再回写（翻转即失效重绘药丸）
            if (HIWORD(wParam) == BN_CLICKED)
            {
                SetToggle((HWND)lParam, !ToggleOn((HWND)lParam));
                SaveFromControls();
            }
            return 0;
        case IDC_EDT_COUNT:
            if (HIWORD(wParam) == EN_CHANGE) SaveFromControls();
            return 0;
        case IDC_CMB_DARK:
        case IDC_CMB_FONTSIZE:
        case IDC_CMB_SKIN:
        case IDC_CMB_UIFONT:
        case IDC_CMB_TOGGLEMODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) SaveFromControls();
            return 0;
        case IDC_BTN_USERDICT:
            CreateUserDictWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            return 0;
        case IDC_BTN_HOTPHRASES:
            ShowHotPhraseWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), hwnd);
            return 0;
        case IDC_BTN_PHRASES:
            ShowPhraseWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), hwnd);
            return 0;
        case IDC_BTN_STATS:
            ShowUsageStatsWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            return 0;
        }
        break;

    case WM_SIZE:
        UpdateSettingsScrollbar(LayoutSettings()); // 缩放→重排→滚动范围同步
        return 0;
    case WM_PAINT:
        PaintSettings(hwnd); // 导航/卡片/标题/分隔线/底部提示统一绘制
        return 0;
    case WM_ERASEBKGND:
    {
        // 中性底一次铺满即可：导航/卡片/文本全部在 WM_PAINT 里绘制
        HDC hdc = (HDC)wParam;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBgBrush);
        return 1;
    }
    case WM_LBUTTONDOWN:
    {
        // 导航项命中→换页：回顶重排全刷（其余区域点击走默认，不夺编辑焦点）
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int navTop = Scaled(56), itemH = Scaled(36);
        if (x >= Scaled(10) && x < Scaled(kNavW) && y >= navTop)
        {
            int idx = (y - navTop) / itemH;
            if (idx >= 0 && idx < (int)g_pages.size() && idx != g_curPage)
            {
                g_curPage = idx;
                { // 记忆导航页（重开设置窗回到上次查看页）
                    Settings st = LoadSettings();
                    if (st.lastSettingsPage != (uint32_t)idx)
                    {
                        st.lastSettingsPage = (uint32_t)idx;
                        SaveSettingsDiff(st);
                    }
                }
                g_scrollY = 0; // 换页回顶
                ApplyPageVisibility();
                UpdateSettingsScrollbar(LayoutSettings());
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        break;
    }
    case WM_MOUSEMOVE:
    {
        // 导航 hover 药丸；离开导航区即时消隐（TrackMouseEvent 挂 WM_MOUSELEAVE）
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int navTop = Scaled(56), itemH = Scaled(36);
        int hover = -1;
        if (x >= Scaled(10) && x < Scaled(kNavW) && y >= navTop)
        {
            int idx = (y - navTop) / itemH;
            if (idx >= 0 && idx < (int)g_pages.size()) hover = idx;
        }
        if (!g_trackingMouse)
        {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) g_trackingMouse = true;
        }
        if (hover != g_navHover)
        {
            g_navHover = hover;
            RECT navRc{ 0, 0, Scaled(kNavW), Scaled(56) + (int)g_pages.size() * itemH };
            InvalidateRect(hwnd, &navRc, FALSE); // 只刷导航列
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (g_navHover != -1)
        {
            g_navHover = -1;
            g_trackingMouse = false;
            RECT navRc{ 0, 0, Scaled(kNavW), Scaled(56) + (int)g_pages.size() * Scaled(36) };
            InvalidateRect(hwnd, &navRc, FALSE);
        }
        return 0;
    case WM_DRAWITEM:
    {
        auto* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlType == ODT_BUTTON)
        {
            if (g_toggleOn.find(dis->hwndItem) != g_toggleOn.end())
            {
                DrawToggle(dis); // 药丸开关（状态外置 g_toggleOn）
                return TRUE;
            }
            for (HWND h : g_accentBtns)
                if (h == dis->hwndItem) { DrawAccentBtn(dis); return TRUE; }
        }
        break;
    }
    case WM_VSCROLL:
    {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int line = MulDiv(30, g_setDpi, 96);
        int pos = (int)si.nPos;
        switch (LOWORD(wParam))
        {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_TOP: pos = 0; break;          // Home/程序化回顶（验证轮发现缺失）
        case SB_BOTTOM:                       // End/程序化到底：目标=可滚最大值
            pos = (int)si.nMax - (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = (int)si.nTrackPos; break;
        default: return 0;
        }
        int maxPos = (int)si.nMax - (int)si.nPage;
        if (maxPos < 0) maxPos = 0;
        if (pos < 0) pos = 0;
        if (pos > maxPos) pos = maxPos;
        if (pos != g_scrollY)
        {
            g_scrollY = pos;
            UpdateSettingsScrollbar(LayoutSettings());
            InvalidateRect(hwnd, NULL, TRUE); // 卡片/控件整体随滚动重绘
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        int delta = (short)HIWORD(wParam); // 正=前滚(向上)
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = g_scrollY - (delta / 40) * MulDiv(30, g_setDpi, 96); // 120→3 行
        int maxPos = (int)si.nMax - (int)si.nPage;
        if (maxPos < 0) maxPos = 0;
        if (pos < 0) pos = 0;
        if (pos > maxPos) pos = maxPos;
        if (pos != g_scrollY)
        {
            g_scrollY = pos;
            UpdateSettingsScrollbar(LayoutSettings());
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        // 宽度下限=最窄可读；高度任意（超出靠纵向滚动）；上限防失控
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = g_setMinW;
        mmi->ptMinTrackSize.y = g_setMinH;
        mmi->ptMaxTrackSize.x = MulDiv(1600, g_setDpi, 96);
        mmi->ptMaxTrackSize.y = MulDiv(2200, g_setDpi, 96);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        // 卡片上的静态文本：白底主文本；行描述（g_descWnds）：白底次级文本
        for (HWND h : g_descWnds)
        {
            if ((HWND)lParam == h)
            {
                SetBkColor((HDC)wParam, kUiCard);
                SetTextColor((HDC)wParam, kUiText2);
                return (LRESULT)g_hCardBrush;
            }
        }
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hCardBrush;
    case WM_CTLCOLORBTN:
        // owner-draw 勾选框背景擦除（默认 BTNFACE 会露灰，须给卡片白刷）
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hCardBrush;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hCardBrush;
    case WM_KEYDOWN:
        // Esc 隐藏设置窗（与标题栏关闭同语义；焦点在子控件时不触发）
        if (wParam == VK_ESCAPE)
        {
            SaveSettingsWindowRect();
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        SaveSettingsWindowRect();
        ShowWindow(hwnd, SW_HIDE); // 关闭=隐藏，保留控件状态
        return 0;
    case WM_DESTROY:
        SaveSettingsWindowRect(); // 进程退出兜底（隐藏路径已存则 diff 零写）
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ---------------- 跨文件共享资源与回调（uiinternal.h）----------------

UiHooks g_hooks; // 全局回调收口（P3）：唯一写点=main.cpp 装配段

HBRUSH g_hBgBrush = nullptr;    // WinUI 中性窗体底
HBRUSH g_hCardBrush = nullptr;  // WinUI 卡片白
HFONT g_hFont = nullptr;        // 12px：通用控件（用户词典窗沿用）

// 设置窗位置/尺寸落盘（隐藏/销毁时调用；重开恢复，免每次居中回默认尺寸）
void SaveSettingsWindowRect()
{
    if (!g_hSettings || !IsWindow(g_hSettings) || IsIconic(g_hSettings)) return;
    RECT wr{};
    GetWindowRect(g_hSettings, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w < 50 || h < 50) return; // 异常态不记录
    Settings st = LoadSettings();
    if (st.setWinX != wr.left || st.setWinY != wr.top ||
        st.setWinW != w || st.setWinH != h)
    {
        st.setWinX = wr.left;
        st.setWinY = wr.top;
        st.setWinW = w;
        st.setWinH = h;
        SaveSettingsDiff(st);
    }
}

// 注册表被托盘等外部路径改写后，若设置窗开着则同步控件（防显示与实际脱节）
void RefreshSettingsControls()
{
    if (g_hChkPinyin) LoadToControls();
}

bool IsSettingsWindowVisible()
{
    return g_hSettings && IsWindowVisible(g_hSettings);
}

void HideSettingsWindow()
{
    if (!g_hSettings) return;
    SaveSettingsWindowRect();
    ShowWindow(g_hSettings, SW_HIDE);
}

void CreateSettingsWindow(HINSTANCE hInst)
{
    if (g_hSettings && IsWindow(g_hSettings))
    {
        LX_LOG(L"ui", L"设置窗复用");
        ShowWindow(g_hSettings, SW_SHOW);
        SetForegroundWindow(g_hSettings);
        return;
    }
    if (g_hSettings)
    {
        // 句柄已失效（窗口曾被销毁）：清空指针走重建，否则复用路径永远静默空转
        LX_LOG(L"ui", L"设置窗句柄失效，重建");
        g_hSettings = nullptr;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!g_hBgBrush)
        g_hBgBrush = CreateSolidBrush(kUiBg);
    if (!g_hCardBrush)
        g_hCardBrush = CreateSolidBrush(kUiCard);
    wc.hbrBackground = g_hBgBrush; // WinUI 中性窗体底；缩放时正确擦除背景
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);

    // 随 DPI 简单缩放（96 基准）；WS_THICKFRAME 可拖拽缩放，WM_SIZE 重排
    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int scale = (int)dpi;
    g_setDpi = (int)dpi;
    int comboListUnits = 8; // 下拉弹开高度=行高倍数（条目多）

    // 关键：创建子控件期间 Edit 会触发 EN_CHANGE、复选框初始为未勾选，若此时
    // 回写会把所有设置污染为 0。整个控件创建期间屏蔽保存，直到 LoadToControls
    // 正确加载后才放开（LoadToControls 结束时会把 g_updating 复位为 false）。
    g_updating = true;
    g_pages.clear();
    g_descWnds.clear();
    g_accentBtns.clear();
    g_curPage = 0;
    g_navHover = -1;

    g_hSettings = CreateWindowExW(WS_EX_COMPOSITED, kSettingsWndClass, L"灵犀输入法设置",
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                   WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   MulDiv(900, scale, 96), MulDiv(680, scale, 96),
                                   NULL, NULL, hInst, NULL);
    if (!g_hSettings)
    {
        LX_LOG(L"ui", L"设置窗创建失败 gle=%u（诊断）", GetLastError());
        g_updating = false;
        return;
    }
    LX_LOG(L"ui", L"设置窗句柄 OK，继续子控件构建（诊断）");

    // 恢复上次位置/尺寸（隐藏时落盘）；无效（未记录/显示器拔了/
    // 分辨率变了）则回退默认尺寸+居中
    {
        Settings stR = LoadSettings();
        bool restored = false;
        if (stR.setWinW >= MulDiv(700, scale, 96) && stR.setWinH >= MulDiv(520, scale, 96))
        {
            RECT wr{ stR.setWinX, stR.setWinY,
                     stR.setWinX + stR.setWinW, stR.setWinY + stR.setWinH };
            HMONITOR mon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            RECT inter;
            if (mon && GetMonitorInfoW(mon, &mi) &&
                IntersectRect(&inter, &wr, &mi.rcWork) &&
                inter.right - inter.left >= 100 && inter.bottom - inter.top >= 60)
            {
                SetWindowPos(g_hSettings, NULL,
                             stR.setWinX, stR.setWinY, stR.setWinW, stR.setWinH,
                             SWP_NOZORDER);
                restored = true;
            }
        }
        if (!restored)
        {
            // 打开即居中于鼠标所在显示器工作区（CW_USEDEFAULT 是级联位，
            // 反复开关逐次偏移；居中固定观感）
            RECT wr{};
            GetWindowRect(g_hSettings, &wr);
            POINT cpt{};
            GetCursorPos(&cpt);
            HMONITOR mon = MonitorFromPoint(cpt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            RECT wa = wr;
            if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
            SetWindowPos(g_hSettings, NULL,
                         wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2,
                         wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }

    auto mkFont = [scale](int px, int weight) {
        return CreateFontW(-MulDiv(px, scale, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };
    g_hFont = mkFont(12, FW_NORMAL);          // 通用控件（用户词典窗沿用）
    g_hFontLabel = mkFont(14, FW_NORMAL);     // 行标题
    g_hFontDesc = mkFont(12, FW_NORMAL);      // 行描述
    g_hFontTitle = mkFont(20, FW_SEMIBOLD);   // 页标题
    g_hFontNav = mkFont(15, FW_SEMIBOLD);     // 导航标题
    g_hFontCardHdr = mkFont(14, FW_SEMIBOLD); // 卡片标题

    // ---- Win11 设置结构：页→卡→行。控件先创建（不可见），mkRow 挂进模型，
    // ApplyPageVisibility 按当前页显隐，LayoutSettings 统一排布 ----
    auto addPage = [&](const wchar_t* nav) {
        g_pages.push_back(SetPage{ nav });
    };
    auto addCard = [&](const wchar_t* header) {
        g_pages.back().cards.push_back(SetCard{ header });
    };
    auto mkRow = [&](const wchar_t* title, const wchar_t* desc, HWND ctl, CtlKind kind, int ctlW) {
        SetRow row;
        row.lab = CreateWindowExW(0, L"STATIC", title,
                                  WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                                  0, 0, 10, 22, g_hSettings, NULL, hInst, NULL);
        if (desc)
        {
            row.desc = CreateWindowExW(0, L"STATIC", desc,
                                       WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                                       0, 0, 10, 18, g_hSettings, NULL, hInst, NULL);
            g_descWnds.push_back(row.desc);
        }
        row.ctl = ctl;
        row.kind = kind;
        row.ctlW = ctlW;
        g_pages.back().cards.back().rows.push_back(row);
    };
    auto mkToggle = [&](int id) -> HWND {
        // 不能写 BS_AUTOCHECKBOX|BS_OWNERDRAW：低位样式位或后=0xB 归并为纯
        // BS_OWNERDRAW，勾选态静默丢失（BM_GETCHECK 恒 0 → 历史刷零真凶）。
        // 状态外置 g_toggleOn，点击在 WM_COMMAND 里翻转。
        HWND h = CreateWindowExW(0, L"BUTTON", L"",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 44, 24, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        g_toggleOn[h] = false;
        return h;
    };
    auto mkCombo = [&](int id, void (*fill)(HWND)) -> HWND {
        HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST,
                                 0, 0, 10, MulDiv(30, scale, 96) * comboListUnits,
                                 g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        if (fill) fill(h);
        return h;
    };
    auto mkEdit = [&](int id) -> HWND {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"9",
                               WS_CHILD | WS_TABSTOP | ES_NUMBER,
                               0, 0, 10, 30, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
    };
    auto mkBtn = [&](const wchar_t* text, int id) -> HWND {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 10, 32, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        g_accentBtns.push_back(h); // 皮肤强调色按钮
        return h;
    };

    // ---- 页 1：常规 ----
    addPage(L"常规");
    addCard(L"输入");
    g_hChkPinyin = mkToggle(IDC_CHK_PINYIN);
    mkRow(L"拼音组合模式", L"字母上屏汉字，空格或数字选择；关闭则字母直通并启用英文补全",
          g_hChkPinyin, CK_TOGGLE, 44);
    g_hChkFuzzy = mkToggle(IDC_CHK_FUZZY);
    mkRow(L"模糊音", L"z/zh、c/ch、s/sh、n/l、前后鼻音互通，精确匹配优先",
          g_hChkFuzzy, CK_TOGGLE, 44);
    { static const wchar_t* fzName[8] = { L"zh 与 z 互通", L"ch 与 c 互通", L"sh 与 s 互通",
        L"n 与 l 互通", L"前鼻音与后鼻音互通", L"f 与 h 互通", L"l 与 r 互通", L"g 与 k 互通" };
      static const int fzId[8] = { IDC_CHK_FZ_ZH, IDC_CHK_FZ_CH, IDC_CHK_FZ_SH, IDC_CHK_FZ_NL, IDC_CHK_FZ_NG,
        IDC_CHK_FZ_FH, IDC_CHK_FZ_LR, IDC_CHK_FZ_GK };
      for (int fz = 0; fz < 8; fz++)
      { g_hChkFz[fz] = mkToggle(fzId[fz]); mkRow(fzName[fz], L"总开关开启时按组生效", g_hChkFz[fz], CK_TOGGLE, 44); } }
    g_hChkShuangpin = mkToggle(IDC_CHK_SHUANGPIN);
    mkRow(L"小鹤双拼", L"每音节两键：声母+韵母，零声母按韵母首字母补齐",
          g_hChkShuangpin, CK_TOGGLE, 44);
    addCard(L"输出");
    g_hChkCnPunct = mkToggle(IDC_CHK_CNPUNCT);
    mkRow(L"中文标点", L". , / ; 等自动转全角，Shift 输出 ？：》《 等",
          g_hChkCnPunct, CK_TOGGLE, 44);
    g_hChkTraditional = mkToggle(IDC_CHK_TRADITIONAL);
    mkRow(L"繁体输出", L"候选简体字转为繁体上屏", g_hChkTraditional, CK_TOGGLE, 44);

    // ---- 页 2：按键 ----
    addPage(L"按键");
    addCard(L"切换");
    auto fillToggle = [](HWND h) {
        for (const wchar_t* s : { L"关闭", L"Shift", L"Shift 或 Ctrl", L"CapsLock", L"Ctrl" })
            ComboBox_AddString(h, s);
    };
    g_hCmbToggleMode = mkCombo(IDC_CMB_TOGGLEMODE, fillToggle);
    mkRow(L"中英切换键", L"单击切换中英文，候选窗显示中/英提示；CapsLock 模式下键盘大小写灯仍会翻转（系统底层行为），介意请用 Shift 模式",
          g_hCmbToggleMode, CK_COMBO, 150);
    addCard(L"符号");
    g_hChkSymbolHotkey = mkToggle(IDC_CHK_SYMBOLHOTKEY);
    mkRow(L"符号面板热键", L"Ctrl+Shift+B 全局独占打开符号面板，默认关闭，也可从托盘打开",
          g_hChkSymbolHotkey, CK_TOGGLE, 44);
    addCard(L"英文补全");
    g_hChkEn = mkToggle(IDC_CHK_EN);
    mkRow(L"英文逐键补全", L"词中自动补全英文单词", g_hChkEn, CK_TOGGLE, 44);
    g_hEdtCount = mkEdit(IDC_EDT_COUNT);
    mkRow(L"补全候选数", L"英文补全显示的候选数量（1-9）", g_hEdtCount, CK_EDIT, 64);

    // ---- 页 3：外观 ----
    addPage(L"外观");
    addCard(L"候选窗");
    auto fillDark = [](HWND h) {
        for (const wchar_t* s : { L"跟随应用", L"浅色", L"深色" })
            ComboBox_AddString(h, s);
    };
    g_hCmbDark = mkCombo(IDC_CMB_DARK, fillDark);
    mkRow(L"明暗", L"候选窗跟随应用主题或强制浅色/深色", g_hCmbDark, CK_COMBO, 150);
    auto fillFont = [](HWND h) {
        for (uint32_t z : {12u, 14u, 16u, 18u, 20u, 24u, 28u, 32u})
        {
            wchar_t b[8];
            _snwprintf_s(b, _TRUNCATE, L"%u", z);
            ComboBox_AddString(h, b);
        }
    };
    g_hCmbFontSize = mkCombo(IDC_CMB_FONTSIZE, fillFont);
    mkRow(L"字号", L"候选词条文字大小", g_hCmbFontSize, CK_COMBO, 100);
    auto fillUiFont = [](HWND h) {
        for (const wchar_t* s : kUiFontNames)
            ComboBox_AddString(h, s);
    };
    g_hCmbUiFont = mkCombo(IDC_CMB_UIFONT, fillUiFont);
    mkRow(L"字体", L"候选窗与浮层文字字体族（下次出候选即时生效）", g_hCmbUiFont, CK_COMBO, 130);
    auto fillSkin = [](HWND h) {
        for (const wchar_t* s : { L"翠绿", L"青碧", L"橙色", L"红色", L"金色", L"茶棕", L"玫粉" })
            ComboBox_AddString(h, s);
    };
    auto fillOpacity = [](HWND h) {
        for (const wchar_t* s : { L"不透明", L"90%", L"80%", L"70%" })
            ComboBox_AddString(h, s);
    };
    g_hCmbOpacity = mkCombo(IDC_CMB_OPACITY, fillOpacity);
    mkRow(L"不透明度", L"候选窗整体透明度（半透明可避免遮挡底下内容）", g_hCmbOpacity, CK_COMBO, 100);
    g_hCmbSkin = mkCombo(IDC_CMB_SKIN, fillSkin);
    mkRow(L"界面配色", L"高亮与强调色皮肤", g_hCmbSkin, CK_COMBO, 100);

    // ---- 页 4：词典 ----
    addPage(L"词典");
    addCard(L"词典与工具");
    HWND hBtnDict = mkBtn(L"打开词典", IDC_BTN_USERDICT);
    mkRow(L"用户词典", L"管理用户词典与自定义词条", hBtnDict, CK_BUTTON, 130);
    HWND hBtnPhrases = mkBtn(L"管理短语", IDC_BTN_HOTPHRASES);
    mkRow(L"快捷短语", L"查看和维护快捷短语", hBtnPhrases, CK_BUTTON, 130);
    HWND hBtnCphrases = mkBtn(L"管理短语", IDC_BTN_PHRASES);
    mkRow(L"自定义短语", L"缩写一打就出长句（如 addr→地址，整串输入触发）",
          hBtnCphrases, CK_BUTTON, 130);
    HWND hBtnStats = mkBtn(L"查看统计", IDC_BTN_STATS);
    mkRow(L"打字统计", L"今日/累计选词次数、上屏汉字数与活跃天数（托盘菜单也可打开）",
          hBtnStats, CK_BUTTON, 130);

    // ---- 页 5：智能 ----
    addPage(L"智能");
    addCard(L"智能特性");
    g_hChkLlmRerank = mkToggle(IDC_CHK_LLMRERANK);
    mkRow(L"LLM 上下文重排", L"实验：停顿打完一字后按上文重排候选，增加 CPU 占用",
          g_hChkLlmRerank, CK_TOGGLE, 44);
    g_hChkLlmMultiPath = mkToggle(IDC_CHK_LLMMULTIPATH);
    mkRow(L"LLM 多路采样", L"实验：贪心+采样两路投票，重排更稳定，进一步增加 CPU 占用",
          g_hChkLlmMultiPath, CK_TOGGLE, 44);
    g_hChkAssocPick = mkToggle(IDC_CHK_ASSOCPICK);
    mkRow(L"选词联想", L"中文上屏后自动弹出联想候选，可点选或空格连续输出；其他按键即撤销",
          g_hChkAssocPick, CK_TOGGLE, 44);

    EnumChildWindows(g_hSettings, [](HWND h, LPARAM lp) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)g_hFontLabel);
    for (HWND h : g_descWnds) // 行描述：次级字号
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontDesc, TRUE);

    // 最小尺寸=左导航+右侧卡片可读下限；默认 900×680
    g_setMinW = MulDiv(700, scale, 96);
    g_setMinH = MulDiv(520, scale, 96);

    LoadToControls();
    // 记忆上次查看页（重开直达；页数随版本变，越界回首页）
    {
        uint32_t lp = LoadSettings().lastSettingsPage;
        if (lp >= g_pages.size()) lp = 0;
        g_curPage = (int)lp;
    }
    ApplyPageVisibility(); // 只显示当前页的行控件
    UpdateSettingsScrollbar(LayoutSettings());
    ShowWindow(g_hSettings, SW_SHOW);
    LX_LOG(L"ui", L"设置窗构建完成并显示（诊断）");
    SetForegroundWindow(g_hSettings);
}

// ---------------- 公共 API（settingsui.h）----------------

void ShowSettingsWindow(HINSTANCE hInst)
{
    CreateSettingsWindow(hInst);
}

} // namespace lingxi
