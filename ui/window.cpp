#include "window.h"

#include <windowsx.h>

#include "../common/proto.h"
#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"
#include "settingsui.h"

namespace lingxi {

static const wchar_t kCandWndClass[] = L"LingxiCandidateWnd";

// 编译时间戳：候选窗右下角可见，用于区分各应用加载的新旧版 DLL
// （TSF 已加载的 DLL 重建后旧进程不换新，必须重启应用）
#define LX_WSTR2(x) L##x
#define LX_WSTR(x) LX_WSTR2(x)
static const wchar_t kBuildStamp[] = LX_WSTR(__DATE__) L" " LX_WSTR(__TIME__);

bool CandWindow::Init(HINSTANCE hInst)
{
    InitializeCriticalSection(&_cs);
    _csInit = true;

    WNDCLASSEXW wc = { sizeof(wc) };
    // 注意：不加 CS_DROPSHADOW——Win11 下它对 DWM 圆角窗算出**方角投影**，
    // 视觉为黑色描边、移动/隐藏后拖出残影（用户实测报告）；浮层阴影交给 DWM。
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = s_WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kCandWndClass;
    if (!RegisterClassExW(&wc))
    {
        LX_LOG(L"ui", L"RegisterClassExW 失败 err=%u", GetLastError());
        return false;
    }

    // WS_EX_NOACTIVATE：鼠标点击候选时宿主应用保持焦点
    // WS_EX_LAYERED：候选窗透明度定制需要分层窗口（LWA_ALPHA）；默认 100% 不透明
    _hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                            kCandWndClass, L"", WS_POPUP,
                            _lastX, _lastY, 240, 120, NULL, NULL, hInst, NULL);
    if (!_hwnd)
    {
        LX_LOG(L"ui", L"CreateWindowExW 失败 err=%u", GetLastError());
        return false;
    }
    SetWindowLongPtrW(_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    // Win11 顶层窗默认自带 1px 深色边框线——在浅色浮层上就是用户报告的「黑描边」。
    UiDisableFrameBorder(_hwnd);
    UiApplyRoundCorners(_hwnd, 240, 120); // Win11 DWM 圆角 / 老系统 RGN

    RebuildFonts(LoadSettings().candidateFontSize);
    ApplySkin(LoadSettings().skinIndex);
    return true;
}

void CandWindow::Shutdown()
{
    if (_hwnd) { DestroyWindow(_hwnd); _hwnd = nullptr; }
    if (_font) { DeleteObject(_font); _font = nullptr; }
    if (_toastFont) { DeleteObject(_toastFont); _toastFont = nullptr; }
    if (_smallFont) { DeleteObject(_smallFont); _smallFont = nullptr; }
    // 刻意不 DeleteCriticalSection：PredictProc/client 线程可能仍在
    // Show/Hide/EnterCriticalSection（Stop 有 2s 超时上限），进程随即退出，
    // 此处删锁会让存活线程踩已删 CS —— 退出竞态崩溃（审计 P1）。泄漏无害。
    _csInit = false;
}

// 按字号（DPI 缩放）重建三支字体；先删旧字体，避免 GDI 泄漏
void CandWindow::RebuildFonts(uint32_t fontSize)
{
    if (fontSize < 12) fontSize = 12;
    if (fontSize > 32) fontSize = 32;
    _fontSize = fontSize;
    LX_LOG(L"ui", L"RebuildFonts fontSize=%u", fontSize);

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
                        L"Microsoft YaHei UI");
    _toastFont = CreateFontW(toastH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei");
    _smallFont = CreateFontW(smallH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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

// 调用方持锁：把 _pageStart 规整到合法范围并返回当前页 [begin,end)。
void CandWindow::CurrentPageRangeLocked(size_t* begin, size_t* end) const
{
    size_t total = _cands.size();
    size_t start = _pageStart;
    if (start > total) start = 0;
    if (total > 0 && start >= total) start = (total - 1) / kPageSize * kPageSize;
    size_t b = start;
    size_t e = min(total, b + kPageSize);
    *begin = b;
    *end = e;
}

void CandWindow::LayoutAndPosition()
{
    int itemH = 0, pad = 0;
    LayoutMetrics(&itemH, &pad);

    // 锁内只取当前页候选快照（避免在锁内调 GetDC/GDI）
    std::vector<std::wstring> page;
    std::wstring composing;
    {
        EnterCriticalSection(&_cs);
        size_t pgBegin = 0, pgEnd = 0;
        CurrentPageRangeLocked(&pgBegin, &pgEnd);
        page.reserve(pgEnd - pgBegin);
        for (size_t i = pgBegin; i < pgEnd; i++) page.push_back(_cands[i]);
        composing = _composing;
        LeaveCriticalSection(&_cs);
    }

    // B2 拼音回显：组合串非空时首行多占一行高
    int compH = composing.empty() ? 0 : itemH;

    _height = pad * 2 + compH + itemH * (int)page.size() + 18; // +18：底部页码/构建标记条

    int textMax = 0;
    HDC hdc = GetDC(_hwnd);
    if (hdc)
    {
        HGDIOBJ old = SelectObject(hdc, _font);
        SIZE sz{};
        for (const auto& s : page)
        {
            GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
            if (sz.cx > textMax) textMax = sz.cx;
        }
        SelectObject(hdc, old);
        ReleaseDC(_hwnd, hdc);
    }
    _width = max(180, textMax + pad * 2 + MulDiv(28, 96, 96) + 24);

    // 锚定策略（用户报告"输入框什么都没有也弹预测、遮住屏幕一角"的体验修）：
    //   caret 有效→caret；caret 退化→**当前鼠标位（注意力所在）**；都拿不到→
    //   上次记忆位。旧实现退到构造期固定 (120,120)/陈旧历史位 = 空 caret 场景
    //   把面板拍在屏幕角落遮挡内容。y 轴补 work-area 钳制（原实现只钳 x，
    //   下翻场景可整体出屏）。
    const bool caretOk = (_caret.right != 0 || _caret.bottom != 0);
    POINT pt{};
    const bool havePt = GetCursorPos(&pt);
    int ax, ay;
    if (caretOk) { ax = _caret.left; ay = _caret.bottom; }
    else if (havePt) { ax = pt.x; ay = pt.y; }
    else { ax = _lastX; ay = _lastY; }

    HMONITOR mon = MonitorFromPoint(POINT{ ax, ay }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi))
    {
        mi.rcWork.left = 0; mi.rcWork.top = 0;
        mi.rcWork.right = 1920; mi.rcWork.bottom = 1040;
    }
    int x = caretOk ? _caret.left : ax + MulDiv(12, 96, 96);
    int y = caretOk ? ay + 4 : ay + MulDiv(24, 96, 96);
    if (caretOk && y + _height > mi.rcWork.bottom)
        y = _caret.top - _height - 4; // 下方放不下翻上方（仅 caret 场景，鼠标位不翻）
    // 最终钳制双轴：任何来源的坐标都不会越出工作区（遮挡/半出屏根治）
    x = max(mi.rcWork.left + 4, min(x, mi.rcWork.right - _width - 4));
    y = max(mi.rcWork.top + 4, min(y, mi.rcWork.bottom - _height - 4));
    _lastX = x;
    _lastY = y;

    SetWindowPos(_hwnd, HWND_TOPMOST, x, y, _width, _height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UiApplyRoundCorners(_hwnd, _width, _height); // RGN 回退路径需随尺寸重设
    ApplyOpacity(); // 透明度设置实时生效（内部按 % 去重，无变化零成本）
    // 移动/缩放后**同步**补画一帧：异步 WM_PAINT 会让新位置先闪露上一帧旧像素
    //（"残留屏幕"残影根因；CS_DROPSHADOW 方影已在 Init 移除，黑描边另一半）。
    RedrawWindow(_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
}

void CandWindow::Show(const RECT& caretScreen, const std::vector<std::wstring>& cands,
                      bool dark, uint32_t ver, const std::wstring& composing)
{
    if (!_hwnd) return;
    if (uint32_t fs = LoadSettings().candidateFontSize; fs != _fontSize)
        RebuildFonts(fs); // 字号设置变化即时生效
    if (uint32_t sk = LoadSettings().skinIndex; sk != _skinIndex)
        ApplySkin(sk); // 皮肤设置变化即时生效
    EnterCriticalSection(&_cs);
    _caret = caretScreen;
    _cands = cands;
    _composing = composing;
    _dark = dark;
    _ver = ver;
    _pageStart = 0; // 新一批候选：回到第一页
    LeaveCriticalSection(&_cs);
    LayoutAndPosition();
    _visible = true;
}

void CandWindow::SetPage(uint32_t ver, uint32_t pageStart)
{
    if (!_hwnd) return;
    bool changed = false;
    EnterCriticalSection(&_cs);
    if (ver == _ver && pageStart < _cands.size() && pageStart != _pageStart)
    {
        _pageStart = pageStart;
        changed = true;
    }
    LeaveCriticalSection(&_cs);
    if (changed)
    {
        LayoutAndPosition();
        _visible = true;
    }
}

void CandWindow::ShowModeToast(bool english)
{
    if (!_hwnd) return;
    bool dark = _dark;
    EnterCriticalSection(&_cs);
    _toastMode = true;
    _toastEnglish = english;
    _dark = dark;
    LeaveCriticalSection(&_cs);

    int w = 78, h = 58;
    int x = _lastX, y = _lastY;
    if (_caret.right != 0 || _caret.bottom != 0)
    {
        x = _caret.left;
        y = _caret.bottom + 4;
    }
    HMONITOR mon = MonitorFromRect(&_caret, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi))
    {
        if (y + h > mi.rcWork.bottom) y = _caret.top - h - 4;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        x = max(mi.rcWork.left, min(x, mi.rcWork.right - w));
    }
    SetWindowPos(_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UiApplyRoundCorners(_hwnd, w, h);
    InvalidateRect(_hwnd, NULL, TRUE);
    SetTimer(_hwnd, 1, 1000, NULL);
}

void CandWindow::Hide()
{
    if (!_hwnd || !_visible) return;
    _visible = false;
    ShowWindow(_hwnd, SW_HIDE);
    EnterCriticalSection(&_cs);
    _cands.clear();
    _composing.clear();
    _pageStart = 0;
    LeaveCriticalSection(&_cs);
}

void CandWindow::ApplyOpacity()
{
    if (!_hwnd) return;
    unsigned op = lingxi::LoadSettings().candidateOpacity;
    if (op < 70) op = 70; if (op > 100) op = 100;
    if ((int)op == _opacityApplied) return;
    _opacityApplied = (int)op;
    SetLayeredWindowAttributes(_hwnd, 0, MulDiv(op, 255, 100), LWA_ALPHA);
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
    {
        EnterCriticalSection(&_cs);
        toast = _toastMode;
        toastEn = _toastEnglish;
        dark = _dark;
        LeaveCriticalSection(&_cs);
    }
    if (toast)
    {
        COLORREF bg = toastEn ? (dark ? RGB(44, 44, 44) : kUiFlyoutBg)
                              : _accent;
        HBRUSH hb = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hb);
        DeleteObject(hb);
        HGDIOBJ old = SelectObject(hdc, _toastFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, toastEn ? (dark ? RGB(228, 228, 228) : kUiText)
                                  : RGB(255, 255, 255));
        DrawTextW(hdc, toastEn ? L"英" : L"中", -1, &rc,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, old);
        COLORREF border = dark ? RGB(66, 66, 66) : kUiFlyoutBorder;
        HBRUSH hbd = CreateSolidBrush(border);
        FrameRect(hdc, &rc, hbd);
        DeleteObject(hbd);
        EndPaint(_hwnd, &ps);
        return;
    }

    std::vector<std::wstring> cands;
    uint32_t pageStart = 0;
    int hoverRow = -1;
    std::wstring composing;
    {
        EnterCriticalSection(&_cs);
        cands = _cands;
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
    for (size_t i = pgBegin; i < pgEnd; i++)
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
    }
    SelectObject(hdc, oldPen);

    // 底部条（小号灰字，不占行高）：
    //   左侧页码（多于一页时显示“‹ 1/7 ›”）；右下角构建标记（看到它=新 DLL）。
    {
        SelectObject(hdc, _smallFont);
        COLORREF dim = dark ? RGB(150, 150, 150) : kUiText2;
        if (totalPages > 1)
        {
            wchar_t pg[32];
            _snwprintf_s(pg, _TRUNCATE, L"\u2039 %zu/%zu \u203a", curPage, totalPages);
            SetTextColor(hdc, dim);
            RECT rcPg = { 6, rc.bottom - 16, rc.right / 2, rc.bottom };
            DrawTextW(hdc, pg, -1, &rcPg, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        }
        // 构建戳只作开发诊断（LINGXI_BUILD_STAMP=1 才画）——用户界面不露版本号
        static const bool sShowStamp = GetEnvironmentVariableW(L"LINGXI_BUILD_STAMP", NULL, 0) > 0;
        if (sShowStamp)
        {
            SetTextColor(hdc, dark ? RGB(120, 120, 120) : RGB(170, 170, 170));
            RECT rcStamp = { rc.right / 2, rc.bottom - 16, rc.right - 4, rc.bottom };
            DrawTextW(hdc, kBuildStamp, -1, &rcStamp, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
        }
        SelectObject(hdc, oldFont);
    }

    SelectObject(hdc, oldFont);
    EndPaint(_hwnd, &ps);
}

LRESULT CALLBACK CandWindow::s_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = (CandWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CandWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_TIMER:
        KillTimer(hwnd, (UINT_PTR)wParam);
        EnterCriticalSection(&_cs);
        _toastMode = false;
        LeaveCriticalSection(&_cs);
        // toast 结束必须走**完整隐藏语义**：裸 SW_HIDE 会留下 _visible=true，
        // 下次 Show 跳过显示路径且旧 toast 大色块作为上一帧闪进候选窗（残影观感）
        if (_visible)
        {
            _visible = false;
            ShowWindow(hwnd, SW_HIDE);
            EnterCriticalSection(&_cs);
            _cands.clear();
            _composing.clear();
            _pageStart = 0;
            LeaveCriticalSection(&_cs);
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        if (!_trackingMouse)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            _trackingMouse = true;
        }
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int itemH = 0, pad = 0;
        LayoutMetrics(&itemH, &pad);
        bool compActive = false;
        {
            EnterCriticalSection(&_cs);
            compActive = !_composing.empty();
            LeaveCriticalSection(&_cs);
        }
        // B2：拼音回显占首行，候选行判定减去该偏移（负值落回显行=无悬停）
        int yOff = pad + (compActive ? itemH : 0);
        int row = (pt.y - yOff) / itemH;
        bool valid = false;
        {
            EnterCriticalSection(&_cs);
            size_t pgBegin = 0, pgEnd = 0;
            CurrentPageRangeLocked(&pgBegin, &pgEnd);
            valid = (row >= 0 && (size_t)row < (pgEnd - pgBegin));
            LeaveCriticalSection(&_cs);
        }
        int newRow = valid ? row : -1;
        if (newRow != _hoverRow)
        {
            _hoverRow = newRow;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        _trackingMouse = false;
        if (_hoverRow != -1)
        {
            _hoverRow = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        // 滚轮翻页（B7）：上滚=上一页、下滚=下一页。Win10+ 默认"悬停非活动
        // 窗口时滚动"把滚轮路由到候选窗（宿主保焦点）；关闭该系统设置的
        // 用户用 PageUp/PageDown、-/= 翻页（DLL 侧既有路径）。锁外重排
        // （LayoutAndPosition 自带锁与 InvalidateRect）。
        int delta = (short)GET_WHEEL_DELTA_WPARAM(wParam);
        int steps = delta / WHEEL_DELTA; // 常规滚轮 ±120；触摸板小增量忽略
        bool changed = false;
        EnterCriticalSection(&_cs);
        size_t total = _cands.size();
        if (total > kPageSize && steps != 0)
        {
            size_t maxStart = (total - 1) / kPageSize * kPageSize;
            long long ns = (long long)_pageStart - (long long)steps * (long long)kPageSize;
            if (ns < 0) ns = 0;
            if (ns > (long long)maxStart) ns = (long long)maxStart;
            if ((size_t)ns != _pageStart)
            {
                _pageStart = (uint32_t)ns; // ns 已夹在 [0, maxStart] 内
                changed = true;
            }
        }
        LeaveCriticalSection(&_cs);
        if (changed)
        {
            _hoverRow = -1; // 翻页后旧行悬停态作废
            LayoutAndPosition();
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int itemH = 0, pad = 0;
        LayoutMetrics(&itemH, &pad);
        bool compActive = false;
        {
            EnterCriticalSection(&_cs);
            compActive = !_composing.empty();
            LeaveCriticalSection(&_cs);
        }
        // B2：拼音回显占首行，点击行判定减去该偏移（点回显行不选中任何候选）
        int yOff = pad + (compActive ? itemH : 0);
        int row = (pt.y - yOff) / itemH; // 页内行号（0 起）

        uint32_t ver = 0;
        uint32_t globalIdx = 0;
        bool hit = false;
        {
            EnterCriticalSection(&_cs);
            size_t pgBegin = 0, pgEnd = 0;
            CurrentPageRangeLocked(&pgBegin, &pgEnd);
            ver = _ver;
            if (row >= 0 && (size_t)row < (pgEnd - pgBegin))
            {
                globalIdx = (uint32_t)(pgBegin + (size_t)row);
                hit = true;
            }
            LeaveCriticalSection(&_cs);
        }
        if (hit && _pick)
        {
            _pick(ver, globalIdx);
            Hide();
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        // 右键候选词 → 词典操作菜单（置顶/提升/降频/删除用户词条）。
        // 命中测试与 WM_LBUTTONUP 同口径：回显行偏移 + 页内行号。
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int itemH = 0, pad = 0;
        LayoutMetrics(&itemH, &pad);
        std::wstring word, composing;
        {
            EnterCriticalSection(&_cs);
            composing = _composing;
            int yOff = pad + (composing.empty() ? 0 : itemH);
            int row = (pt.y - yOff) / itemH;
            size_t pgBegin = 0, pgEnd = 0;
            CurrentPageRangeLocked(&pgBegin, &pgEnd);
            if (row >= 0 && (size_t)row < (pgEnd - pgBegin))
                word = _cands[pgBegin + (size_t)row];
            LeaveCriticalSection(&_cs);
        }
        if (word.empty()) return 0; // 点在回显行/页外：无菜单

        bool inDict = UserDictHasWord(word);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"置顶该词");
        AppendMenuW(menu, MF_STRING, 2, L"提升词频");
        AppendMenuW(menu, MF_STRING | (inDict ? 0 : MF_GRAYED), 3, L"降低词频");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (inDict ? 0 : MF_GRAYED), 4, L"从用户词典删除");
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        SetForegroundWindow(hwnd); // 菜单在外部点击时正确消失
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                 screen.x, screen.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        if (cmd == 1 || cmd == 2 || cmd == 3 || cmd == 4)
        {
            CandDictOp op = (cmd == 1) ? CandDictOp::Pin
                        : (cmd == 2) ? CandDictOp::Boost
                        : (cmd == 3) ? CandDictOp::Lower : CandDictOp::Delete;
            // 失败仅记日志不打断输入（词典窗提供手动键兜底）；成功后下一次
            // 击键即见新排序（引擎经 g_reloadUserDict 已重载）。
            ApplyCandidateDictOp(word, composing, op);
        }
        return 0;
    }

    case WM_DESTROY:
        _hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace lingxi
