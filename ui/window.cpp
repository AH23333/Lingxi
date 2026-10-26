#include "window.h"

#include <windowsx.h>

#include "../common/log.h"

namespace lingxi {

static const wchar_t kCandWndClass[] = L"LingxiCandidateWnd";

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
    return true;
}

void CandWindow::Shutdown()
{
    if (_hwnd) { DestroyWindow(_hwnd); _hwnd = nullptr; }
    if (_font) { DeleteObject(_font); _font = nullptr; }
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

void CandWindow::LayoutAndPosition()
{
    int itemH = 0, pad = 0;
    LayoutMetrics(&itemH, &pad);

    _height = pad * 2 + itemH * (int)_cands.size();

    int textMax = 0;
    HDC hdc = GetDC(_hwnd);
    if (hdc)
    {
        HGDIOBJ old = SelectObject(hdc, _font);
        SIZE sz{};
        for (const auto& s : _cands)
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
    LeaveCriticalSection(&_cs);
    LayoutAndPosition();
    _visible = true;
}

void CandWindow::Hide()
{
    if (!_hwnd || !_visible) return;
    _visible = false;
    ShowWindow(_hwnd, SW_HIDE);
    EnterCriticalSection(&_cs);
    _cands.clear();
    LeaveCriticalSection(&_cs);
}

void CandWindow::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(_hwnd, &ps);
    if (!hdc) return;

    std::vector<std::wstring> cands;
    bool dark = true;
    {
        EnterCriticalSection(&_cs);
        cands = _cands;
        dark = _dark;
        LeaveCriticalSection(&_cs);
    }

    RECT rc;
    GetClientRect(_hwnd, &rc);

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

    for (int i = 0; i < (int)cands.size(); i++)
    {
        RECT row = { pad, pad + i * itemH, rc.right - pad, pad + (i + 1) * itemH };
        if (i == 0)
        {
            HBRUSH hAccent = CreateSolidBrush(accent);
            FillRect(hdc, &row, hAccent);
            DeleteObject(hAccent);
            SetTextColor(hdc, RGB(255, 255, 255));
        }
        else
        {
            SetTextColor(hdc, fg);
        }
        wchar_t buf[512];
        _snwprintf_s(buf, _TRUNCATE, L"%d  %s", (i + 1) % 10, cands[i].c_str());
        DrawTextW(hdc, buf, -1, &row, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
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

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int itemH = 0, pad = 0;
        LayoutMetrics(&itemH, &pad);
        int row = (pt.y - pad) / itemH;

        uint32_t ver = 0;
        int count = 0;
        {
            EnterCriticalSection(&_cs);
            ver = _ver;
            count = (int)_cands.size();
            LeaveCriticalSection(&_cs);
        }
        if (row >= 0 && row < count && _pick)
        {
            _pick(ver, (uint32_t)row);
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
