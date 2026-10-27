#include "window.h"

#include <windowsx.h>

#include "../common/proto.h"
#include "../common/log.h"

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
    _hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kCandWndClass, L"", WS_POPUP,
                            _lastX, _lastY, 240, 120, NULL, NULL, hInst, NULL);
    if (!_hwnd)
    {
        LX_LOG(L"ui", L"CreateWindowExW 失败 err=%u", GetLastError());
        return false;
    }
    SetWindowLongPtrW(_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    _font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    _toastFont = CreateFontW(34, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    _smallFont = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return true;
}

void CandWindow::Shutdown()
{
    if (_hwnd) { DestroyWindow(_hwnd); _hwnd = nullptr; }
    if (_font) { DeleteObject(_font); _font = nullptr; }
    if (_toastFont) { DeleteObject(_toastFont); _toastFont = nullptr; }
    if (_smallFont) { DeleteObject(_smallFont); _smallFont = nullptr; }
    if (_csInit) { DeleteCriticalSection(&_cs); _csInit = false; }
}

void CandWindow::LayoutMetrics(int* itemH, int* pad)
{
    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    *itemH = MulDiv(30, dpi, 96);
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
    {
        EnterCriticalSection(&_cs);
        size_t pgBegin = 0, pgEnd = 0;
        CurrentPageRangeLocked(&pgBegin, &pgEnd);
        page.reserve(pgEnd - pgBegin);
        for (size_t i = pgBegin; i < pgEnd; i++) page.push_back(_cands[i]);
        LeaveCriticalSection(&_cs);
    }

    _height = pad * 2 + itemH * (int)page.size() + 18; // +18：底部页码/构建标记条

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

    int x = _caret.left, y = _caret.bottom + 4;
    if (_caret.right == 0 && _caret.bottom == 0)
    {
        x = _lastX;
        y = _lastY;
    }
    HMONITOR mon = MonitorFromRect(&_caret, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi))
    {
        if (y + _height > mi.rcWork.bottom)
            y = _caret.top - _height - 4;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        x = max(mi.rcWork.left, min(x, mi.rcWork.right - _width));
    }
    _lastX = x;
    _lastY = y;

    SetWindowPos(_hwnd, HWND_TOPMOST, x, y, _width, _height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(_hwnd, NULL, TRUE);
}

void CandWindow::Show(const RECT& caretScreen, const std::vector<std::wstring>& cands,
                      bool dark, uint32_t ver)
{
    if (!_hwnd) return;
    EnterCriticalSection(&_cs);
    _caret = caretScreen;
    _cands = cands;
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
    _pageStart = 0;
    LeaveCriticalSection(&_cs);
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
        COLORREF bg = toastEn ? (dark ? RGB(60, 60, 60) : RGB(250, 250, 250))
                             : RGB(0, 120, 215);
        HBRUSH hb = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hb);
        DeleteObject(hb);
        HGDIOBJ old = SelectObject(hdc, _toastFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, toastEn ? (dark ? RGB(225, 225, 225) : RGB(50, 50, 50))
                                 : RGB(255, 255, 255));
        DrawTextW(hdc, toastEn ? L"英" : L"中", -1, &rc,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, old);
        COLORREF border = dark ? RGB(90, 90, 90) : RGB(200, 200, 200);
        HBRUSH hbd = CreateSolidBrush(border);
        FrameRect(hdc, &rc, hbd);
        DeleteObject(hbd);
        EndPaint(_hwnd, &ps);
        return;
    }

    std::vector<std::wstring> cands;
    uint32_t pageStart = 0;
    int hoverRow = -1;
    {
        EnterCriticalSection(&_cs);
        cands = _cands;
        pageStart = _pageStart;
        hoverRow = _hoverRow;
        LeaveCriticalSection(&_cs);
    }

    size_t total = cands.size();
    size_t pgBegin = pageStart;
    if (pgBegin >= total) pgBegin = 0; // 容错：异常 pageStart 回首页
    size_t pgEnd = min(total, pgBegin + kPageSize);
    size_t totalPages = total ? (total + kPageSize - 1) / kPageSize : 1;
    size_t curPage = pgBegin / kPageSize + 1;

    COLORREF bg = dark ? RGB(43, 43, 43) : RGB(250, 250, 250);
    COLORREF fg = dark ? RGB(222, 222, 222) : RGB(32, 32, 32);
    COLORREF border = dark ? RGB(80, 80, 80) : RGB(200, 200, 200);
    COLORREF accent = RGB(0, 120, 215);

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

    for (size_t i = pgBegin; i < pgEnd; i++)
    {
        size_t row = i - pgBegin;
        RECT rowR = { pad, pad + (int)row * itemH, rc.right - pad, pad + (int)(row + 1) * itemH };
        if (row == 0)
        {
            HBRUSH hAccent = CreateSolidBrush(accent);
            FillRect(hdc, &rowR, hAccent);
            DeleteObject(hAccent);
            SetTextColor(hdc, RGB(255, 255, 255));
        }
        else if ((int)row == hoverRow)
        {
            COLORREF hoverBg = dark ? RGB(64, 64, 64) : RGB(224, 238, 252);
            HBRUSH hHover = CreateSolidBrush(hoverBg);
            FillRect(hdc, &rowR, hHover);
            DeleteObject(hHover);
            SetTextColor(hdc, fg);
        }
        else
        {
            SetTextColor(hdc, fg);
        }
        wchar_t buf[512];
        _snwprintf_s(buf, _TRUNCATE, L"%zu  %s", row + 1, cands[i].c_str());
        DrawTextW(hdc, buf, -1, &rowR, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    // 底部条（小号灰字，不占行高）：
    //   左侧页码（多于一页时显示“‹ 1/7 ›”）；右下角构建标记（看到它=新 DLL）。
    {
        SelectObject(hdc, _smallFont);
        COLORREF dim = dark ? RGB(150, 150, 150) : RGB(130, 130, 130);
        if (totalPages > 1)
        {
            wchar_t pg[32];
            _snwprintf_s(pg, _TRUNCATE, L"\u2039 %zu/%zu \u203a", curPage, totalPages);
            SetTextColor(hdc, dim);
            RECT rcPg = { 6, rc.bottom - 16, rc.right / 2, rc.bottom };
            DrawTextW(hdc, pg, -1, &rcPg, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        }
        SetTextColor(hdc, dark ? RGB(120, 120, 120) : RGB(170, 170, 170));
        RECT rcStamp = { rc.right / 2, rc.bottom - 16, rc.right - 4, rc.bottom };
        DrawTextW(hdc, kBuildStamp, -1, &rcStamp, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
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
        ShowWindow(hwnd, SW_HIDE);
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
        int row = (pt.y - pad) / itemH;
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

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int itemH = 0, pad = 0;
        LayoutMetrics(&itemH, &pad);
        int row = (pt.y - pad) / itemH; // 页内行号（0 起）

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

    case WM_DESTROY:
        _hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace lingxi
