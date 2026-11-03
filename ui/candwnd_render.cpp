#include "window.h"

#include "candwnd_internal.h"

#include "../common/proto.h"
#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

//
// 候选窗绘制（P2 拆分自 window.cpp，2026-10-31）：皮肤/字体/度量、
// tooltip 浮窗、OnPaint（候选行/拼音回显/toast/底部工具条）。
//

namespace lingxi {

// 编译时间戳：候选窗右下角可见，用于区分各应用加载的新旧版 DLL
// （TSF 已加载的 DLL 重建后旧进程不换新，必须重启应用）
#define LX_WSTR2(x) L##x
#define LX_WSTR(x) LX_WSTR2(x)
static const wchar_t kBuildStamp[] = LX_WSTR(__DATE__) L" " LX_WSTR(__TIME__);

// 按字号（DPI 缩放）重建三支字体；先删旧字体，避免 GDI 泄漏
void CandWindow::RebuildFonts(uint32_t fontSize)
{
    if (fontSize < 12) fontSize = 12;
    if (fontSize > 32) fontSize = 32;
    _fontSize = fontSize;
    // 字体族按设置（界面字体族下拉）；小字（页码/工具条）默认 Segoe UI，
    // 非默认族时跟随所选族保持观感一致
    uint32_t faceIdx = LoadSettings().uiFont;
    if (faceIdx >= kUiFontCount) faceIdx = 0;
    _fontFace = faceIdx;
    const wchar_t* face = kUiFontFaces[faceIdx];
    const wchar_t* smallFace = faceIdx == 0 ? L"Segoe UI" : face;
    LX_LOG(L"ui", L"RebuildFonts fontSize=%u face=%ls", fontSize, face);

    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                       "GetDpiForSystem"))
        dpi = pGetDpi();

    int mainH = -MulDiv((int)fontSize, dpi, 96);
    int toastH = MulDiv((int)fontSize * 2, dpi, 96);
    int smallH = -MulDiv((int)fontSize * 5 / 8, dpi, 96);

    if (_font) { DeleteObject(_font); _font = nullptr; }
    if (_toastFont) { DeleteObject(_toastFont); _toastFont = nullptr; }
    if (_smallFont) { DeleteObject(_smallFont); _smallFont = nullptr; }

    _font = CreateFontW(mainH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                        face);
    _toastFont = CreateFontW(toastH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             face);
    _smallFont = CreateFontW(smallH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, smallFace);
}

// 应用皮肤（共享 uistyle.h 皮肤表）；越界回退默认翠绿
void CandWindow::ApplySkin(uint32_t idx)
{
    if (idx >= kSkinCount) idx = 0;
    _skinIndex = idx;
    _accent = UiSkin(idx).accent;
    _hoverLight = UiSkin(idx).hoverLight;
    LX_LOG(L"ui", L"ApplySkin skin=%u", idx);
}

void CandWindow::LayoutMetrics(int* itemH, int* pad)
{
    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    uint32_t fs = LoadSettings().candidateFontSize;
    // 行高约 1.875 倍字号（原 30 对应字号 16），保证大字不被截断
    *itemH = MulDiv((int)fs * 15 / 8, dpi, 96);
    *pad = MulDiv(8, dpi, 96);
}

int CandWindow::ToolbarHeight()
{
    return Scale96(24);
}

void CandWindow::ShowTip(const wchar_t* text, const POINT& clientPt)
{
    if (!text || !*text || !_hwnd) return;
    if (!_ttHwnd)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = s_TipWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"LingxiCandTip";
        RegisterClassW(&wc);
        _ttHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  L"LingxiCandTip", L"", WS_POPUP,
                                  0, 0, 10, 10, _hwnd, NULL, NULL, NULL);
        if (!_ttHwnd) return;
        SetWindowLongPtrW(_ttHwnd, GWLP_USERDATA, (LONG_PTR)this);
    }
    wcsncpy_s(_ttText, text, _TRUNCATE);
    int w = Scale96(40), h = Scale96(20);
    HDC hdc = GetDC(_ttHwnd);
    if (hdc)
    {
        HGDIOBJ old = SelectObject(hdc, _smallFont);
        SIZE sz{};
        if (GetTextExtentPoint32W(hdc, _ttText, (int)wcslen(_ttText), &sz))
        {
            w = sz.cx + Scale96(16);
            h = sz.cy + Scale96(8);
        }
        SelectObject(hdc, old);
        ReleaseDC(_ttHwnd, hdc);
    }
    POINT sp = clientPt;
    ClientToScreen(_hwnd, &sp);
    int x = sp.x + Scale96(14);
    int y = sp.y + Scale96(18);
    HMONITOR mon = MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi))
    {
        if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
        if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
        x = max(mi.rcWork.left, x);
        y = max(mi.rcWork.top, y);
    }
    SetWindowPos(_ttHwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(_ttHwnd, NULL, TRUE);
}

void CandWindow::HideTip()
{
    if (_ttHwnd) ShowWindow(_ttHwnd, SW_HIDE); // 常驻隐藏复用，不 DestroyWindow
}

LRESULT CALLBACK CandWindow::s_TipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT)
    {
        auto* self = (CandWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc && self)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            bool dark = true;
            EnterCriticalSection(&self->_cs);
            dark = self->_dark;
            LeaveCriticalSection(&self->_cs);
            HBRUSH hb = CreateSolidBrush(dark ? RGB(60, 60, 60) : kUiCard);
            FillRect(hdc, &rc, hb);
            DeleteObject(hb);
            HBRUSH hbd = CreateSolidBrush(dark ? RGB(84, 84, 84) : kUiFlyoutBorder);
            FrameRect(hdc, &rc, hbd);
            DeleteObject(hbd);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, dark ? RGB(228, 228, 228) : kUiText);
            HGDIOBJ old = SelectObject(hdc, self->_smallFont);
            DrawTextW(hdc, self->_ttText, -1, &rc,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(hdc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CandWindow::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(_hwnd, &ps);
    if (!hdc) return;

    RECT rc;
    GetClientRect(_hwnd, &rc);

    // 中/英文模式提示气泡：居中大字，1 秒后由 WM_TIMER 关闭
    bool toast = false, toastEn = false, dark = true;
    std::wstring toastText;
    {
        EnterCriticalSection(&_cs);
        toast = _toastMode;
        toastEn = _toastEnglish;
        dark = _dark;
        toastText = _toastText; // 非空=文字气泡（R8 目标达成提醒等）
        LeaveCriticalSection(&_cs);
    }
    if (toast)
    {
        bool textToast = !toastText.empty();
        COLORREF bg = textToast ? (dark ? RGB(44, 44, 44) : kUiFlyoutBg)
                      : toastEn ? (dark ? RGB(44, 44, 44) : kUiFlyoutBg)
                                : _accent;
        HBRUSH hb = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hb);
        DeleteObject(hb);
        HGDIOBJ old = SelectObject(hdc, _toastFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textToast ? (dark ? RGB(228, 228, 228) : kUiText)
                      : toastEn ? (dark ? RGB(228, 228, 228) : kUiText)
                                : RGB(255, 255, 255));
        DrawTextW(hdc, textToast ? toastText.c_str()
                  : toastEn ? L"英" : L"中", -1, &rc,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | (textToast ? DT_END_ELLIPSIS : 0));
        SelectObject(hdc, old);
        COLORREF border = dark ? RGB(66, 66, 66) : kUiFlyoutBorder;
        HBRUSH hbd = CreateSolidBrush(border);
        FrameRect(hdc, &rc, hbd);
        DeleteObject(hbd);
        EndPaint(_hwnd, &ps);
        return;
    }

    std::vector<std::wstring> cands;
    std::vector<std::wstring> anno;
    uint32_t pageStart = 0;
    int hoverRow = -1;
    std::wstring composing;
    {
        EnterCriticalSection(&_cs);
        cands = _cands;
        anno = _anno;
        pageStart = _pageStart;
        hoverRow = _hoverRow;
        composing = _composing;
        LeaveCriticalSection(&_cs);
    }

    size_t total = cands.size();
    size_t pgBegin = pageStart;
    if (pgBegin >= total) pgBegin = 0; // 容错：异常 pageStart 回首页
    size_t pgEnd = min(total, pgBegin + kPageSize);
    size_t totalPages = total ? (total + kPageSize - 1) / kPageSize : 1;
    size_t curPage = pgBegin / kPageSize + 1;

    // WinUI 浮层配色：深色 #2C2C2C / 浅色近白，中性描边，强调色只用于选中
    COLORREF bg = dark ? RGB(44, 44, 44) : kUiFlyoutBg;
    COLORREF fg = dark ? RGB(228, 228, 228) : kUiText;
    COLORREF border = dark ? RGB(66, 66, 66) : kUiFlyoutBorder;
    COLORREF accent = _accent;
    COLORREF annoFg = dark ? RGB(170, 170, 170) : kUiText2; // GAP R6 注音次级色

    HBRUSH hBg = CreateSolidBrush(bg);
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);
    HBRUSH hBorder = CreateSolidBrush(border);
    FrameRect(hdc, &rc, hBorder);
    DeleteObject(hBorder);

    int itemH = 0, pad = 0;
    LayoutMetrics(&itemH, &pad);

    HGDIOBJ oldFont = SelectObject(hdc, _font);
    SetBkMode(hdc, TRANSPARENT);

    // B2 拼音回显首行：底色与候选区微区分（GDI 无 alpha，用一档实色近似半透明），
    // 文本 = 组合串 + "："；不参与 hover/点击（WM_MOUSEMOVE/LBUTTONUP 已做行偏移）。
    int compH = composing.empty() ? 0 : itemH;
    if (compH)
    {
        RECT compR = { pad, pad, rc.right - pad, pad + compH };
        HBRUSH hComp = CreateSolidBrush(dark ? RGB(54, 54, 54) : kUiBg);
        FillRect(hdc, &compR, hComp);
        DeleteObject(hComp);
        std::wstring comp = composing + L"\xFF1A";
        SetTextColor(hdc, dark ? RGB(180, 180, 180) : _accent);
        HGDIOBJ oldCompFont = SelectObject(hdc, _font); // 此前未选字体=系统默认小字（走查发现）
        RECT compText = { pad, pad, rc.right - pad, pad + compH };
        {
            // 超长掐头保尾：用户关心刚敲的尾部拼音，截头保留 + "…" 前缀
            const int avail = compText.right - compText.left;
            SIZE szc{};
            GetTextExtentPoint32W(hdc, comp.c_str(), (int)comp.size(), &szc);
            if (szc.cx > avail)
            {
                const std::wstring full = comp;
                const wchar_t* ell = L"\u2026";
                for (size_t keep = comp.size() - 1; keep >= 1; keep--)
                {
                    std::wstring trial = std::wstring(ell) + full.substr(full.size() - keep);
                    GetTextExtentPoint32W(hdc, trial.c_str(), (int)trial.size(), &szc);
                    if (szc.cx <= avail) { comp = trial; break; }
                }
            }
            DrawTextW(hdc, comp.c_str(), -1, &compText,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        }
        SelectObject(hdc, oldCompFont);
    }

    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    // C 横排分支：单行左起列排（列边界 _colX=LayoutAndPosition 单一事实源，
    // 二者同 UI 线程，无锁读安全）。选中/hover 色块、序号+词+行内注音与
    // 竖排行同语义，仅几何不同；词 8 字截断与布局侧同口径。
    // 横排度量（列内边距/行高）为纸面推算——待用户重启后真机确认。
    if (_horizontal && _colX.size() == pgEnd - pgBegin + 1)
    {
        const int y0 = pad + compH;
        const int colIn = Scale96(6); // 列内文本左缩进（布局 colPad 的一半）
        for (size_t i = pgBegin; i < pgEnd; i++)
        {
            size_t row = i - pgBegin;
            RECT colR = { _colX[row], y0, _colX[row + 1], y0 + itemH };
            HBRUSH hCol = nullptr;
            if (row == 0)
            {
                hCol = CreateSolidBrush(accent);
                SetTextColor(hdc, RGB(255, 255, 255));
            }
            else if ((int)row == hoverRow)
            {
                COLORREF hoverBg = dark ? RGB(64, 64, 64) : _hoverLight;
                hCol = CreateSolidBrush(hoverBg);
                SetTextColor(hdc, fg);
            }
            else
            {
                SetTextColor(hdc, fg);
            }
            if (hCol)
            {
                HGDIOBJ oldBrush = SelectObject(hdc, hCol);
                RECT rr = colR;
                InflateRect(&rr, -1, -1);
                RoundRect(hdc, rr.left, rr.top, rr.right + 1, rr.bottom + 1, 8, 8);
                SelectObject(hdc, oldBrush);
                DeleteObject(hCol);
            }
            std::wstring word = cands[i];
            if (word.size() > 8) word = word.substr(0, 8) + L"\u2026";
            wchar_t buf[512];
            _snwprintf_s(buf, _TRUNCATE, L"%zu  %s", row + 1, word.c_str());
            RECT tR = { colR.left + colIn, colR.top, colR.right, colR.bottom };
            DrawTextW(hdc, buf, -1, &tR, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            if (i < anno.size() && !anno[i].empty())
            {
                SIZE szMain{};
                GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &szMain);
                wchar_t abuf[512];
                _snwprintf_s(abuf, _TRUNCATE, L"（%s）", anno[i].c_str());
                SetTextColor(hdc, row == 0 ? RGB(240, 240, 240) : annoFg);
                RECT aR = { tR.left + szMain.cx, colR.top, colR.right, colR.bottom };
                DrawTextW(hdc, abuf, -1, &aR, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
        }
    }
    else for (size_t i = pgBegin; i < pgEnd; i++)
    {
        size_t row = i - pgBegin;
        RECT rowR = { pad, pad + compH + (int)row * itemH, rc.right - pad,
                      pad + compH + (int)(row + 1) * itemH };
        // WinUI 列表项：选中/hover 为 4px 圆角色块（内缩 1px 呼吸）
        HBRUSH hRow = nullptr;
        if (row == 0)
        {
            hRow = CreateSolidBrush(accent);
            SetTextColor(hdc, RGB(255, 255, 255));
        }
        else if ((int)row == hoverRow)
        {
            COLORREF hoverBg = dark ? RGB(64, 64, 64) : _hoverLight;
            hRow = CreateSolidBrush(hoverBg);
            SetTextColor(hdc, fg);
        }
        else
        {
            SetTextColor(hdc, fg);
        }
        if (hRow)
        {
            HGDIOBJ oldBrush = SelectObject(hdc, hRow);
            RECT rr = rowR;
            InflateRect(&rr, -1, -1);
            RoundRect(hdc, rr.left, rr.top, rr.right + 1, rr.bottom + 1, 8, 8);
            SelectObject(hdc, oldBrush);
            DeleteObject(hRow);
        }
        wchar_t buf[512];
        _snwprintf_s(buf, _TRUNCATE, L"%zu  %s", row + 1, cands[i].c_str());
        DrawTextW(hdc, buf, -1, &rowR, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        // GAP R6 候选注音：主文本后行内绘制（同 _font，宽度已在 LayoutAndPosition
        // 实测计入）；灰阶次级色，选中行近白保对比
        if (i < anno.size() && !anno[i].empty())
        {
            SIZE szMain{};
            GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &szMain);
            wchar_t abuf[512];
            _snwprintf_s(abuf, _TRUNCATE, L"（%s）", anno[i].c_str());
            SetTextColor(hdc, row == 0 ? RGB(240, 240, 240) : annoFg);
            RECT aR = { rowR.left + szMain.cx, rowR.top, rowR.right, rowR.bottom };
            DrawTextW(hdc, abuf, -1, &aR, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        }
    }
    SelectObject(hdc, oldPen);

    // 底部工具条：分隔线 + [‹ 页码 ›]（多页时）…… [固][符][设] + 构建戳（开发诊断）。
    // 按钮 hover/激活态与候选行同风格；固=位置固定生效中（强调色实底）。
    {
        static const bool sShowStamp = GetEnvironmentVariableW(L"LINGXI_BUILD_STAMP", NULL, 0) > 0;
        int tbH = ToolbarHeight();
        int top = rc.bottom - tbH;
        {
            HPEN sep = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ op = SelectObject(hdc, sep);
            MoveToEx(hdc, rc.left, top, NULL);
            LineTo(hdc, rc.right, top);
            SelectObject(hdc, op);
            DeleteObject(sep);
        }
        RECT rPrev, rPg, rNext, rPin, rSym, rSet;
        CalcToolbarRects(rc.right, rc.bottom, tbH, totalPages > 1,
                         &rPrev, &rPg, &rNext, &rPin, &rSym, &rSet);
        bool pinned = LoadSettings().candidatePinned != 0;
        COLORREF dim = dark ? RGB(150, 150, 150) : kUiText2;
        COLORREF dis = dark ? RGB(110, 110, 110) : RGB(176, 176, 176);
        HGDIOBJ oldSmall = SelectObject(hdc, _smallFont);
        auto drawBtn = [&](RECT r, const wchar_t* glyph, int id, bool disabled, bool active)
        {
            if (r.right <= r.left) return;
            if (active)
            {
                HBRUSH hb = CreateSolidBrush(accent);
                HGDIOBJ ob = SelectObject(hdc, hb);
                RoundRect(hdc, r.left, r.top, r.right + 1, r.bottom + 1, 6, 6);
                SelectObject(hdc, ob);
                DeleteObject(hb);
                SetTextColor(hdc, RGB(255, 255, 255));
            }
            else if (id == _hoverBtn && !disabled)
            {
                HBRUSH hb = CreateSolidBrush(dark ? RGB(64, 64, 64) : _hoverLight);
                HGDIOBJ ob = SelectObject(hdc, hb);
                RoundRect(hdc, r.left, r.top, r.right + 1, r.bottom + 1, 6, 6);
                SelectObject(hdc, ob);
                DeleteObject(hb);
                SetTextColor(hdc, fg);
            }
            else
            {
                SetTextColor(hdc, disabled ? dis : dim);
            }
            DrawTextW(hdc, glyph, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        };
        bool firstPage = pgBegin == 0;
        bool lastPage = pgEnd >= total;
        drawBtn(rPrev, L"\u2039", (int)Btn::PrevPage, firstPage, false);
        drawBtn(rNext, L"\u203a", (int)Btn::NextPage, lastPage, false);
        if (totalPages > 1)
        {
            wchar_t pg[32];
            _snwprintf_s(pg, _TRUNCATE, L"%zu/%zu", curPage, totalPages);
            SetTextColor(hdc, dim);
            DrawTextW(hdc, pg, -1, &rPg, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        drawBtn(rPin, L"固", (int)Btn::Pin, false, pinned);
        drawBtn(rSym, L"符", (int)Btn::Symbols, false, false);
        drawBtn(rSet, L"设", (int)Btn::Settings, false, false);
        // 构建戳只作开发诊断（LINGXI_BUILD_STAMP=1 才画）——用户界面不露版本号
        if (sShowStamp)
        {
            SetTextColor(hdc, dark ? RGB(120, 120, 120) : RGB(170, 170, 170));
            RECT rcStamp = { rNext.right + Scale96(8), top, rPin.left - Scale96(8), rc.bottom };
            DrawTextW(hdc, kBuildStamp, -1, &rcStamp,
                      DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        SelectObject(hdc, oldSmall);
    }

    SelectObject(hdc, oldFont);
    EndPaint(_hwnd, &ps);
}

} // namespace lingxi
