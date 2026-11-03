#include "window.h"

#include "candwnd_internal.h"

#include <windowsx.h>
#include <shellapi.h> // ShellExecuteW（右键菜单"联网搜索"）

#include "../common/proto.h"
#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"
#include "settingsui.h"
#include "blockedui.h" // 右键"屏蔽该词"（BlockWord/UnblockWord/IsWordBlocked）

//
// 候选窗交互（P2 拆分自 window.cpp，2026-10-31）：鼠标手势（拖拽/悬停/
// 滚轮/点选）、工具条按钮命中与点击、右键菜单（词典操作/换肤/复制）。
// 消息由 candwnd_core.cpp 的 WndProc 壳分发到本文件 On* 方法。
//

namespace lingxi {

static const wchar_t* TipTextFor(CandWindow::Btn b, bool pinned)
{
    switch (b)
    {
    case CandWindow::Btn::PrevPage:  return L"上一页（滚轮也可翻页）";
    case CandWindow::Btn::NextPage:  return L"下一页（滚轮也可翻页）";
    case CandWindow::Btn::Pin:       return pinned ? L"左键解除固定，恢复跟随光标"
                                                   : L"固定候选窗位置（也可按住本窗拖动）";
    case CandWindow::Btn::Symbols:   return L"符号面板（Ctrl+Shift+B 可开热键）";
    case CandWindow::Btn::Settings:  return L"设置（右键换肤）";
    default: return L"";
    }
}

// 复制文本到剪贴板（右键菜单"复制该词"；CF_UNICODETEXT，失败静默）。
// OpenClipboard 可能被其他进程短暂占用（0x580 访问被拒）——3×30ms 重试，
// 复制场景（用户主动点击）偶发占用下不必让用户重试第二次。
static void CopyTextToClipboard(HWND owner, const std::wstring& s)
{
    if (s.empty()) return;
    bool opened = false;
    for (int i = 0; i < 3 && !opened; i++)
    {
        if (OpenClipboard(owner)) { opened = true; break; }
        Sleep(30);
    }
    if (!opened) return;
    EmptyClipboard();
    SIZE_T cb = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cb);
    if (h)
    {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p)
        {
            wmemcpy(p, s.c_str(), s.size() + 1);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h))
                GlobalFree(h); // 成功后系统接管所有权，失败才需自清
        }
        else
        {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

// 联网搜索该词（Bing；UTF-8 + 百分号编码，中文字符安全进 URL）
static void OpenWebSearch(const std::wstring& word)
{
    if (word.empty()) return;
    int cb = WideCharToMultiByte(CP_UTF8, 0, word.c_str(), (int)word.size(),
                                 NULL, 0, NULL, NULL);
    if (cb <= 0) return;
    std::string u8((size_t)cb, '\0');
    WideCharToMultiByte(CP_UTF8, 0, word.c_str(), (int)word.size(),
                        &u8[0], cb, NULL, NULL);
    std::wstring q;
    q.reserve(u8.size() * 3);
    for (unsigned char c : u8)
    {
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (keep) { q += (wchar_t)c; }
        else { wchar_t b[8]; swprintf_s(b, 8, L"%%%02X", (int)c); q += b; }
    }
    std::wstring url = L"https://www.bing.com/search?q=" + q;
    ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// C 横排列命中：客户区 x → 页内列号（列边界 _colX=LayoutAndPosition 写入，
// 同 UI 线程无锁读）。未命中/竖排返回 -1。
int CandWindow::HitCol(int x) const
{
    if (_colX.size() < 2) return -1;
    for (size_t i = 0; i + 1 < _colX.size(); i++)
        if (x >= _colX[i] && x < _colX[i + 1]) return (int)i;
    return -1;
}

CandWindow::Btn CandWindow::HitTestButton(int x, int y)
{
    if (!_hwnd) return Btn::None;
    RECT rc{};
    GetClientRect(_hwnd, &rc);
    int tbH = ToolbarHeight();
    if (y < rc.bottom - tbH) return Btn::None; // 工具条带之外
    size_t total = 0;
    {
        EnterCriticalSection(&_cs);
        total = _cands.size();
        LeaveCriticalSection(&_cs);
    }
    RECT rPrev, rPg, rNext, rPin, rSym, rSet;
    CalcToolbarRects(rc.right, rc.bottom, tbH, total > _pageSize,
                     &rPrev, &rPg, &rNext, &rPin, &rSym, &rSet);
    auto in = [&x, &y](const RECT& r)
    {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    };
    if (in(rPrev)) return Btn::PrevPage;
    if (in(rNext)) return Btn::NextPage;
    if (in(rPin)) return Btn::Pin;
    if (in(rSym)) return Btn::Symbols;
    if (in(rSet)) return Btn::Settings;
    return Btn::None;
}

LRESULT CandWindow::OnMouseMove(HWND hwnd, LPARAM lParam)
{
    if (_dragging)
    {
        // 拖拽移动：跟随光标，钳制到所在显示器工作区（半出屏不可能）
        POINT cur{};
        GetCursorPos(&cur);
        int x = cur.x - _dragOff.x;
        int y = cur.y - _dragOff.y;
        HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        RECT lim = { -32000, -32000, 32000, 32000 };
        if (GetMonitorInfoW(mon, &mi)) lim = mi.rcWork;
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;
        x = max(lim.left, min(x, lim.right - w));
        y = max(lim.top, min(y, lim.bottom - h));
        SetWindowPos(hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        _dragMoved = true;
        return 0;
    }
    if (!_trackingMouse)
    {
        TRACKMOUSEEVENT tme{ sizeof(tme) };
        tme.dwFlags = TME_LEAVE | TME_HOVER; // HOVER：工具条按钮 tooltip
        tme.dwHoverTime = 400;
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
    // 工具条按钮 hover（先于行 hover；命中按钮时行 hover 自然失效）。
    // 换按钮时重挂 TME_HOVER：hover 是一次性的，不重挂则 tooltip 只出第一次
    int btn = (int)HitTestButton(pt.x, pt.y);
    if (btn != _hoverBtn)
    {
        _hoverBtn = btn;
        if (btn == (int)Btn::None) HideTip();
        else
        {
            TRACKMOUSEEVENT t2{ sizeof(t2) };
            t2.dwFlags = TME_HOVER | TME_LEAVE;
            t2.dwHoverTime = 400;
            t2.hwndTrack = hwnd;
            TrackMouseEvent(&t2);
        }
        InvalidateRect(hwnd, NULL, FALSE);
    }
    // B2：拼音回显占首行，候选行判定减去该偏移（负值落回显行=无悬停）
    int yOff = pad + (compActive ? itemH : 0);
    // C 横排：列命中（x 落列边界、y 落单行带内）；竖排：原行号算式不变
    int row = _horizontal ? ((pt.y >= yOff && pt.y < yOff + itemH) ? HitCol(pt.x) : -1)
                          : (pt.y - yOff) / itemH;
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
        // 换行：旧行 tooltip 作废 + 重挂 TME_HOVER（每行独立 400ms 停留，
        // 截断行 tooltip 才能逐行触发；与按钮 hover 的重挂同型）
        HideTip();
        TRACKMOUSEEVENT t3{ sizeof(t3) };
        t3.dwFlags = TME_HOVER | TME_LEAVE;
        t3.dwHoverTime = 400;
        t3.hwndTrack = hwnd;
        TrackMouseEvent(&t3);
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
}

LRESULT CandWindow::OnMouseHover(HWND hwnd, LPARAM lParam)
{
    // 工具条按钮悬停 400ms → tooltip（悬到别处/点击/翻页即隐）
    if (_hoverBtn != (int)Btn::None)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ShowTip(TipTextFor((Btn)_hoverBtn, LoadSettings().candidatePinned != 0), pt);
    }
    else if (_hoverRow >= 0)
    {
        // 截断候选行悬停：tooltip 显示完整词条（长词 END_ELLIPSIS 截断时的补救）
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        std::wstring word;
        {
            EnterCriticalSection(&_cs);
            size_t pgBegin = 0, pgEnd = 0;
            CurrentPageRangeLocked(&pgBegin, &pgEnd);
            if ((size_t)_hoverRow < (pgEnd - pgBegin))
                word = _cands[pgBegin + (size_t)_hoverRow];
            LeaveCriticalSection(&_cs);
        }
        if (!word.empty() && _hwnd)
        {
            // 与 OnPaint 同口径测宽：文本 = "页内序号␣␣词条"，行宽 = _width-2*pad
            int pad2 = 0;
            {
                int itemH2 = 0;
                LayoutMetrics(&itemH2, &pad2);
            }
            int textW = -1;
            HDC hdc = GetDC(_hwnd);
            if (hdc)
            {
                wchar_t buf[512];
                _snwprintf_s(buf, _TRUNCATE, L"%d  %s", _hoverRow + 1,
                             word.c_str());
                HGDIOBJ old = SelectObject(hdc, _font);
                SIZE sz{};
                if (GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz))
                    textW = sz.cx;
                SelectObject(hdc, old);
                ReleaseDC(_hwnd, hdc);
            }
            if (textW > _width - 2 * pad2)
                ShowTip(word.c_str(), pt); // 完整词条（tooltip 自带 END_ELLIPSIS 兜底）
        }
    }
    // TME_HOVER 一次性：改挂 TME_LEAVE，防每 400ms 重复触发
    TRACKMOUSEEVENT tme{ sizeof(tme) };
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
    return 0;
}

LRESULT CandWindow::OnMouseLeave(HWND hwnd)
{
    _trackingMouse = false;
    HideTip();
    if (_hoverBtn != (int)Btn::None)
    {
        _hoverBtn = (int)Btn::None;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    if (_hoverRow != -1)
    {
        _hoverRow = -1;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
}

LRESULT CandWindow::OnLButtonDown(HWND hwnd, LPARAM lParam)
{
    // toast（中/英气泡）期间不可拖拽/点击：防止把固定位置写到 toast 小窗坐标
    {
        bool toastActive = false;
        EnterCriticalSection(&_cs);
        toastActive = _toastMode;
        LeaveCriticalSection(&_cs);
        if (toastActive) return 0;
    }
    // 按钮按下不启动拖拽（点击在 UP 处理，与候选行选词语义一致）
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (HitTestButton(pt.x, pt.y) != Btn::None) return 0;
    // 拼音回显行 / 工具条空白 = 拖拽手柄（松开时位置写入固定设置）
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int itemH = 0, pad = 0;
    LayoutMetrics(&itemH, &pad);
    bool compActive = false;
    {
        EnterCriticalSection(&_cs);
        compActive = !_composing.empty();
        LeaveCriticalSection(&_cs);
    }
    bool inEcho = compActive && pt.y >= pad && pt.y < pad + itemH;
    bool inTb = pt.y >= rc.bottom - ToolbarHeight();
    if (inEcho || inTb)
    {
        POINT cur{};
        GetCursorPos(&cur);
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        _dragging = true;
        _dragMoved = false;
        _dragOff = { cur.x - wr.left, cur.y - wr.top };
        SetCapture(hwnd);
        HideTip();
    }
    return 0;
}

BOOL CandWindow::OnSetCursor(HWND hwnd, LPARAM lParam)
{
    if (LOWORD(lParam) == HTCLIENT)
    {
        DWORD pos = GetMessagePos();
        POINT pt = { GET_X_LPARAM((LONG)pos), GET_Y_LPARAM((LONG)pos) };
        ScreenToClient(hwnd, &pt);
        if (HitTestButton(pt.x, pt.y) != Btn::None)
        {
            SetCursor(LoadCursorW(NULL, IDC_HAND)); // 按钮：手型
            return TRUE;
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int itemH = 0, pad = 0;
        LayoutMetrics(&itemH, &pad);
        bool compActive = false;
        {
            EnterCriticalSection(&_cs);
            compActive = !_composing.empty();
            LeaveCriticalSection(&_cs);
        }
        bool inEcho = compActive && pt.y >= pad && pt.y < pad + itemH;
        if (inEcho || pt.y >= rc.bottom - ToolbarHeight())
        {
            SetCursor(LoadCursorW(NULL, IDC_SIZEALL)); // 拖拽手柄：四向箭头
            return TRUE;
        }
        if (pt.y >= pad) // 候选行区：可点选（手型暗示，与符号面板一致）
        {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }
    return FALSE; // 非客户区：由 WndProc 落 DefWindowProc
}

LRESULT CandWindow::OnMouseWheel(WPARAM wParam)
{
    // Shift+滚轮：候选窗不透明度 ±5%（70–100 钳制；即时生效+写设置）。
    // 与 Ctrl+滚轮（字号）同款"修饰键占位滚轮"手势，互不冲突。
    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        Settings st = LoadSettings();
        int op = (int)st.candidateOpacity +
                 ((short)GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 5 : -5);
        if (op < 70) op = 70;
        if (op > 100) op = 100;
        if ((uint32_t)op != st.candidateOpacity)
        {
            st.candidateOpacity = (uint32_t)op;
            SaveSettingsDiff(st);
            ApplyOpacity();
            LX_LOG(L"ui", L"Shift+滚轮调候选窗不透明度=%d%%", op);
        }
        return 0;
    }
    // Ctrl+滚轮：候选字号 ±1（12–32 钳制；即时生效+写设置+重排）。
    // 先于翻页判定——Ctrl 按住时滚轮不再翻页（浏览器缩放手势同款占位）。
    if (GetKeyState(VK_CONTROL) & 0x8000)
    {
        Settings st = LoadSettings();
        int fs = (int)st.candidateFontSize +
                 ((short)GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1);
        if (fs < 12) fs = 12;
        if (fs > 32) fs = 32;
        if ((uint32_t)fs != st.candidateFontSize)
        {
            st.candidateFontSize = (uint32_t)fs;
            SaveSettingsDiff(st);
            RebuildFonts(st.candidateFontSize);
            LayoutAndPosition(); // 行高随字号变，立即重排重绘
            LX_LOG(L"ui", L"Ctrl+滚轮调候选字号=%d", fs);
        }
        return 0;
    }
    // 滚轮翻页（B7）：上滚=上一页、下滚=下一页。Win10+ 默认"悬停非活动
    // 窗口时滚动"把滚轮路由到候选窗（宿主保焦点）；关闭该系统设置的
    // 用户用 PageUp/PageDown、-/= 翻页（DLL 侧既有路径）。锁外重排
    // （LayoutAndPosition 自带锁与 InvalidateRect）。
    int delta = (short)GET_WHEEL_DELTA_WPARAM(wParam);
    int steps = delta / WHEEL_DELTA; // 常规滚轮 ±120；触摸板小增量忽略
    bool changed = false;
    EnterCriticalSection(&_cs);
    size_t total = _cands.size();
    if (total > _pageSize && steps != 0)
    {
        size_t maxStart = (total - 1) / _pageSize * _pageSize;
        long long ns = (long long)_pageStart - (long long)steps * (long long)_pageSize;
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

LRESULT CandWindow::OnLButtonUp(HWND hwnd, LPARAM lParam)
{
    // 拖拽收尾：有实际位移 → 当前位置写入"固定位置"设置（松手即固定，
    // "固"按钮点亮；解除用 固左键/右键）
    if (_dragging)
    {
        ReleaseCapture();
        _dragging = false;
        if (_dragMoved)
        {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            Settings st = LoadSettings();
            st.candidatePinned = 1;
            st.candidatePinX = wr.left;
            st.candidatePinY = wr.top;
            SaveSettingsDiff(st);
            _lastX = wr.left;
            _lastY = wr.top;
            InvalidateRect(hwnd, NULL, TRUE); // "固"按钮点亮
            LX_LOG(L"ui", L"拖拽固定候选窗 x=%d y=%d", wr.left, wr.top);
        }
        return 0;
    }
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    HideTip();
    // 工具条按钮：‹/› 翻页、固=固定位置开关、符/设=main 侧入口
    {
        Btn btn = HitTestButton(pt.x, pt.y);
        if (btn == Btn::PrevPage) { DoPageStep(-1); return 0; }
        if (btn == Btn::NextPage) { DoPageStep(1); return 0; }
        if (btn == Btn::Pin)
        {
            Settings st = LoadSettings();
            if (st.candidatePinned)
            {
                st.candidatePinned = 0;
            }
            else
            {
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                st.candidatePinned = 1;
                st.candidatePinX = wr.left;
                st.candidatePinY = wr.top;
            }
            SaveSettingsDiff(st);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (btn == Btn::Symbols || btn == Btn::Settings)
        {
            if (_btnCmd) _btnCmd(btn);
            return 0;
        }
    }
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
    // C 横排：列命中；竖排：原行号算式不变
    int row = _horizontal ? ((pt.y >= yOff && pt.y < yOff + itemH) ? HitCol(pt.x) : -1)
                          : (pt.y - yOff) / itemH; // 页内行号（0 起）

    uint32_t ver = 0;
    uint32_t globalIdx = 0;
    bool hit = false;
    std::wstring word;
    {
        EnterCriticalSection(&_cs);
        size_t pgBegin = 0, pgEnd = 0;
        CurrentPageRangeLocked(&pgBegin, &pgEnd);
        ver = _ver;
        if (row >= 0 && (size_t)row < (pgEnd - pgBegin))
        {
            globalIdx = (uint32_t)(pgBegin + (size_t)row);
            word = _cands[pgBegin + (size_t)row];
            hit = true;
        }
        LeaveCriticalSection(&_cs);
    }
    // 修饰键+左键（功率键，默认左键选词不变）：Ctrl+Shift=删除词条、
    // Ctrl=联网搜索、Shift=复制。按钮点击已在上方 return；修饰键点击
    // 不选词、不清组合。
    if (hit && (GetKeyState(VK_CONTROL) & 0x8000) &&
        (GetKeyState(VK_SHIFT) & 0x8000))
    {
        if (ApplyCandidateDictOp(word, L"", CandDictOp::Delete)) // 删除按键内容匹配，无需键提示
        {
            std::wstring fb = L"已从词典删除：" + word;
            ShowTip(fb.c_str(), pt); // 浮窗反馈（选词不刷新场景防重复点击）
            SetTimer(hwnd, 2, 1500, NULL);
        }
        return 0;
    }
    if (hit && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        OpenWebSearch(word);
        return 0;
    }
    if (hit && (GetKeyState(VK_SHIFT) & 0x8000))
    {
        CopyTextToClipboard(hwnd, word);
        return 0;
    }
    if (hit && _pick)
    {
        _pick(ver, globalIdx);
        Hide();
    }
    return 0;
}

LRESULT CandWindow::OnRButtonUp(HWND hwnd, LPARAM lParam)
{
    // 右键工具条按钮：固=解除固定（恢复跟随光标）、设=循环换肤（免进设置页）
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    {
        Btn btn = HitTestButton(pt.x, pt.y);
        if (btn == Btn::Pin)
        {
            Settings st = LoadSettings();
            if (st.candidatePinned)
            {
                st.candidatePinned = 0;
                SaveSettingsDiff(st);
                InvalidateRect(hwnd, NULL, TRUE);
                LX_LOG(L"ui", L"右键固按钮解除位置固定");
            }
            return 0;
        }
        if (btn == Btn::Settings)
        {
            Settings st = LoadSettings();
            st.skinIndex = (st.skinIndex + 1) % kSkinCount;
            SaveSettingsDiff(st);
            ApplySkin(st.skinIndex);
            InvalidateRect(hwnd, NULL, TRUE);
            LX_LOG(L"ui", L"右键设按钮换肤 skin=%u", st.skinIndex);
            return 0;
        }
    }
    // 右键工具条空白 = 皮肤直选子菜单（7 款勾选直选，免进设置页/免循环试错；
    // 设按钮右键的"循环换肤"保留为盲切快捷）。同款不抢前台：本窗 NOACTIVATE。
    {
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        if (pt.y >= rcClient.bottom - ToolbarHeight())
        {
            Settings st = LoadSettings();
            static const wchar_t* kSkinNames[] = {
                L"翠绿", L"青碧", L"橙色", L"红色", L"金色", L"茶棕", L"玫粉" };
            HMENU menu = CreatePopupMenu();
            for (int i = 0; i < (int)kSkinCount; i++)
                AppendMenuW(menu,
                            MF_STRING | (i == (int)st.skinIndex ? MF_CHECKED : 0),
                            100 + i, kSkinNames[i]);
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                     screen.x, screen.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            if (cmd >= 100 && cmd < 100 + (int)kSkinCount)
            {
                st.skinIndex = (uint32_t)(cmd - 100);
                SaveSettingsDiff(st);
                ApplySkin(st.skinIndex);
                InvalidateRect(hwnd, NULL, TRUE);
                LX_LOG(L"ui", L"工具条右键换肤 skin=%u", st.skinIndex);
            }
            return 0;
        }
    }
    // 右键候选词 → 词典操作菜单（置顶/提升/降频/删除用户词条 + 复制/联网搜索）。
    // 命中测试与 WM_LBUTTONUP 同口径：回显行偏移 + 页内行号。
    int itemH = 0, pad = 0;
    LayoutMetrics(&itemH, &pad);
    std::wstring word, composing;
    {
        EnterCriticalSection(&_cs);
        composing = _composing;
        int yOff = pad + (composing.empty() ? 0 : itemH);
        // C 横排：列命中；竖排：原行号算式不变
        int row = _horizontal ? ((pt.y >= yOff && pt.y < yOff + itemH) ? HitCol(pt.x) : -1)
                              : (pt.y - yOff) / itemH;
        size_t pgBegin = 0, pgEnd = 0;
        CurrentPageRangeLocked(&pgBegin, &pgEnd);
        if (row >= 0 && (size_t)row < (pgEnd - pgBegin))
            word = _cands[pgBegin + (size_t)row];
        LeaveCriticalSection(&_cs);
    }
    if (word.empty())
    {
        // 右键拼音回显行 = 复制拼音串（组合串整段；回显行无词典菜单）
        if (!composing.empty() && pt.y >= pad && pt.y < pad + itemH)
        {
            CopyTextToClipboard(hwnd, composing);
            LX_LOG(L"ui", L"右键回显行复制拼音 len=%zu", composing.size());
        }
        return 0; // 点在回显行/页外/工具条：无菜单
    }

    bool inDict = UserDictHasWord(word);
    bool blocked = IsWordBlocked(word);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"置顶该词");
    AppendMenuW(menu, MF_STRING, 2, L"提升词频");
    AppendMenuW(menu, MF_STRING | (inDict ? 0 : MF_GRAYED), 3, L"降低词频");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (inDict ? 0 : MF_GRAYED), 4, L"从用户词典删除");
    AppendMenuW(menu, MF_STRING | (blocked ? MF_CHECKED : 0), 8,
                blocked ? L"取消屏蔽（恢复候选）" : L"屏蔽该词（不再出现）");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 5, L"复制该词");
    AppendMenuW(menu, MF_STRING, 6, L"联网搜索该词");
    AppendMenuW(menu, MF_STRING, 7, L"查该词拼音");
    POINT screen = pt;
    ClientToScreen(hwnd, &screen);
    // 不做 SetForegroundWindow：本窗 WS_EX_NOACTIVATE，右键菜单用属主窗口
    // 原前台身份直接 TrackPopupMenu。旧实现抢前台会把键盘焦点从宿主拽到
    // 候选窗 → TSF 焦点离开宿主 → SendHide 词框消失（用户报"右键词框
    // 消失"根因）；菜单外部点击消失由系统按前台属主自行处理（Win10/11）。
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                             screen.x, screen.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    if (cmd == 5)
    {
        CopyTextToClipboard(hwnd, word);
        return 0;
    }
    if (cmd == 6)
    {
        OpenWebSearch(word);
        return 0;
    }
    if (cmd == 7)
    {
        // 查拼音：推断全拼浮窗 2.5s（比操作反馈稍长，读拼音要时间）
        std::wstring py = InferWordPinyin(word);
        std::wstring fb = py.empty() ? L"未能推断拼音（非汉字词？）"
                                     : L"拼音：" + py;
        ShowTip(fb.c_str(), pt);
        SetTimer(hwnd, 2, 2500, NULL);
        return 0;
    }
    if (cmd == 8)
    {
        // 屏蔽/取消屏蔽（R6 闭环：blocked.txt 与用户词典同通道热载，下次击键生效）
        std::wstring fb;
        if (blocked)
        {
            fb = UnblockWord(word) ? L"已取消屏蔽，恢复参与候选：" + word
                                   : L"操作失败（该词不在屏蔽列表或文件被占用）";
        }
        else
        {
            switch (BlockWord(word))
            {
            case BlockWordResult::Blocked:
                fb = L"已屏蔽，该词不再出现：" + word; break;
            case BlockWordResult::AlreadyBlocked:
                fb = L"该词已在屏蔽列表中"; break;
            case BlockWordResult::LimitReached:
                fb = L"屏蔽词已达上限（500），请在管理窗清理"; break;
            case BlockWordResult::TooLong:
                fb = L"词过长（上限 50 字符），无法屏蔽"; break;
            case BlockWordResult::SaveFailed:
                fb = L"屏蔽失败（文件被占用？）"; break;
            }
        }
        ShowTip(fb.c_str(), pt);
        SetTimer(hwnd, 2, 1500, NULL);
        return 0;
    }
    if (cmd == 1 || cmd == 2 || cmd == 3 || cmd == 4)
    {
        CandDictOp op = (cmd == 1) ? CandDictOp::Pin
                    : (cmd == 2) ? CandDictOp::Boost
                    : (cmd == 3) ? CandDictOp::Lower : CandDictOp::Delete;
        // 失败仅记日志不打断输入（词典窗提供手动键兜底）；成功后下一次
        // 击键即见新排序（引擎经 g_hooks.reloadUserDict 已重载）。
        if (ApplyCandidateDictOp(word, composing, op))
        {
            // 成功反馈浮窗 1.5s（选词不刷新场景下告知"已生效"，防重复点击）
            static const wchar_t* kOpDone[] = {
                L"已置顶", L"已提升词频", L"已降低词频", L"已从词典删除" };
            std::wstring fb = std::wstring(kOpDone[cmd - 1]) + L"：" + word;
            ShowTip(fb.c_str(), pt);
            SetTimer(hwnd, 2, 1500, NULL);
        }
    }
    return 0;
}

LRESULT CandWindow::OnXButtonUp(WPARAM wParam)
{
    // 鼠标侧键翻页：后退键=上一页、前进键=下一页（与滚轮/‹›同夹取语义）
    int xb = GET_XBUTTON_WPARAM(wParam);
    if (xb == XBUTTON1) DoPageStep(-1);
    else if (xb == XBUTTON2) DoPageStep(1);
    return TRUE; // 已处理：不给宿主留重复导航语义
}

LRESULT CandWindow::OnMButtonUp(HWND hwnd, LPARAM lParam)
{
    // 中键点击候选 = 置顶该词（免开右键菜单的功率键；与菜单"置顶该词"同路）
    {
        bool toastActive = false;
        EnterCriticalSection(&_cs);
        toastActive = _toastMode;
        LeaveCriticalSection(&_cs);
        if (toastActive) return 0;
    }
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    int itemH = 0, pad = 0;
    LayoutMetrics(&itemH, &pad);
    std::wstring word, composing;
    {
        EnterCriticalSection(&_cs);
        composing = _composing;
        int yOff = pad + (composing.empty() ? 0 : itemH);
        // C 横排：列命中；竖排：原行号算式不变
        int row = _horizontal ? ((pt.y >= yOff && pt.y < yOff + itemH) ? HitCol(pt.x) : -1)
                              : (pt.y - yOff) / itemH;
        size_t pgBegin = 0, pgEnd = 0;
        CurrentPageRangeLocked(&pgBegin, &pgEnd);
        if (row >= 0 && (size_t)row < (pgEnd - pgBegin))
            word = _cands[pgBegin + (size_t)row];
        LeaveCriticalSection(&_cs);
    }
    if (word.empty()) return 0;
    // 中键功率键全家桶：默认=置顶；Shift=降低词频；Ctrl=删除词条
    CandDictOp mop = CandDictOp::Pin;
    const wchar_t* mfb = L"已置顶";
    if (GetKeyState(VK_SHIFT) & 0x8000) { mop = CandDictOp::Lower; mfb = L"已降低词频"; }
    else if (GetKeyState(VK_CONTROL) & 0x8000) { mop = CandDictOp::Delete; mfb = L"已从词典删除"; }
    if (ApplyCandidateDictOp(word, composing, mop))
    {
        std::wstring fb = std::wstring(mfb) + L"：" + word;
        ShowTip(fb.c_str(), pt); // 成功反馈浮窗 1.5s（与右键菜单路径同款）
        SetTimer(hwnd, 2, 1500, NULL);
    }
    LX_LOG(L"ui", L"中键词典操作 op=%d word=%ls", (int)mop, word.c_str());
    return 0;
}

} // namespace lingxi
