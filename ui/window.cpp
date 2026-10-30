#include "window.h"

#include <windowsx.h>
#include <shellapi.h> // ShellExecuteW（右键菜单"联网搜索"）

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

// ---- 底部工具条几何（绘制与命中共用单一定义；单位 96dpi，内部 DPI 缩放）----
// 左组（多页时）：‹(20) 页码(46) ›(20)，起于 x=6；
// 右组：固(28) 符(28) 设(28)，止于 x=右-6；中段空白=拖拽手柄。
static UINT Dpi()
{
    UINT dpi = 96;
    if (auto p = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                 "GetDpiForSystem"))
        dpi = p();
    return dpi;
}

static int Scale96(int v) { return MulDiv(v, (int)Dpi(), 96); }

static void CalcToolbarRects(int cw, int ch, int tbH, bool paged,
                             RECT* rPrev, RECT* rPg, RECT* rNext,
                             RECT* rPin, RECT* rSym, RECT* rSet)
{
    int top = ch - tbH + 2;
    int bot = ch - 2;
    auto mk = [top, bot](int x96, int w96) -> RECT
    {
        int x = Scale96(x96), w = Scale96(w96);
        return RECT{ x, top, x + w, bot };
    };
    RECT empty{ 0, 0, 0, 0 };
    if (rPrev) *rPrev = paged ? mk(6, 20) : empty;
    if (rPg)   *rPg   = paged ? mk(28, 46) : empty;
    if (rNext) *rNext = paged ? mk(76, 20) : empty;
    // 右组从右往左：设(0) 符(1) 固(2)，像素直接换算（cw 已是缩放后像素）
    int m = Scale96(6), bw = Scale96(28), g = Scale96(2);
    auto rgt = [&](int fromRight) -> RECT
    {
        int x = cw - m - bw - fromRight * (bw + g);
        return RECT{ x, top, x + bw, bot };
    };
    if (rPin)  *rPin  = rgt(2);
    if (rSym)  *rSym  = rgt(1);
    if (rSet)  *rSet  = rgt(0);
}

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

    _height = pad * 2 + compH + itemH * (int)page.size() + ToolbarHeight(); // 底部工具条

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
    _width = max(200, textMax + pad * 2 + MulDiv(28, 96, 96) + 24); // ≥200：工具条两组按钮排开

    // 位置固定模式（"固"按钮/拖拽设置）：不再跟随光标，钳制到所在显示器工作区
    // （分辨率变化/拔显示器不丢窗）。固定坐标注册表持久化，重启宿主仍生效。
    Settings stPin = LoadSettings();
    if (stPin.candidatePinned)
    {
        int x = stPin.candidatePinX;
        int y = stPin.candidatePinY;
        HMONITOR mon = MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi))
        {
            x = max(mi.rcWork.left + 4, min(x, mi.rcWork.right - _width - 4));
            y = max(mi.rcWork.top + 4, min(y, mi.rcWork.bottom - _height - 4));
        }
        _lastX = x;
        _lastY = y;
        SetWindowPos(_hwnd, HWND_TOPMOST, x, y, _width, _height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UiApplyRoundCorners(_hwnd, _width, _height);
        ApplyOpacity();
        RedrawWindow(_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        return;
    }

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
    else if (LoadSettings().uiFont != _fontFace)
        RebuildFonts(_fontSize); // 字体族设置变化即时生效（字号不变只换 face）
    if (uint32_t sk = LoadSettings().skinIndex; sk != _skinIndex)
        ApplySkin(sk); // 皮肤设置变化即时生效
    EnterCriticalSection(&_cs);
    _caret = caretScreen;
    _cands = cands;
    _composing = composing;
    _dark = dark;
    // 同版本再 Show（重排写回）= 同批候选刷新：保页；新版本（ver 变）才归零
    // 回首页——与 DLL 侧 OnCandidates 的 rerank 判定对称，防"翻页中弹回页 1"。
    if (_ver != ver || _pageStart >= _cands.size()) _pageStart = 0;
    _ver = ver;
    // toast（中/英切换气泡）占用本窗期间候选帧到达：清 toast 态 + 杀 1s 定时器，
    // 让候选立即接管绘制——否则 1s 内候选帧被画成"中/英"大气泡，定时器到期
    // 再把刚到的候选整窗清光（用户报"切换提示吃掉候选"根因）。
    _toastMode = false;
    LeaveCriticalSection(&_cs);
    KillTimer(_hwnd, 1);
    HideTip(); // 候选帧接管窗口（跨线程：tooltip 仅 ShowWindow 隐藏，安全）
    LayoutAndPosition();
    _visible = true;
}

void CandWindow::SetPage(uint32_t ver, uint32_t pageStart)
{
    if (!_hwnd) return;
    bool changed = false;
    EnterCriticalSection(&_cs);
    // 与 Show() 同因：翻页指令期间 toast 态必须让位（清态+杀定时器）
    _toastMode = false;
    if (ver == _ver && pageStart < _cands.size() && pageStart != _pageStart)
    {
        _pageStart = pageStart;
        changed = true;
    }
    LeaveCriticalSection(&_cs);
    KillTimer(_hwnd, 1);
    HideTip();
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
    HideTip();
    SetTimer(_hwnd, 1, 1000, NULL);
}

void CandWindow::Hide()
{
    if (!_hwnd || !_visible) return;
    _visible = false;
    HideTip();
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

int CandWindow::ToolbarHeight()
{
    return Scale96(24);
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
    CalcToolbarRects(rc.right, rc.bottom, tbH, total > kPageSize,
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

void CandWindow::DoPageStep(int steps)
{
    if (!_hwnd || steps == 0) return;
    bool changed = false;
    EnterCriticalSection(&_cs);
    size_t total = _cands.size();
    if (total > kPageSize)
    {
        size_t maxStart = (total - 1) / kPageSize * kPageSize;
        long long ns = (long long)_pageStart + (long long)steps * (long long)kPageSize;
        if (ns < 0) ns = 0;
        if (ns > (long long)maxStart) ns = (long long)maxStart;
        if ((size_t)ns != _pageStart)
        {
            _pageStart = (uint32_t)ns;
            changed = true;
        }
    }
    LeaveCriticalSection(&_cs);
    if (changed)
    {
        _hoverRow = -1; // 翻页后旧行悬停态作废
        LayoutAndPosition();
    }
}

void CandWindow::TogglePin()
{
    if (!_hwnd) return;
    Settings st = LoadSettings();
    if (st.candidatePinned)
    {
        st.candidatePinned = 0;
    }
    else
    {
        RECT wr{};
        GetWindowRect(_hwnd, &wr);
        st.candidatePinned = 1;
        st.candidatePinX = wr.left;
        st.candidatePinY = wr.top;
    }
    SaveSettingsDiff(st);
    InvalidateRect(_hwnd, NULL, TRUE); // "固"按钮点亮/熄灭
    LX_LOG(L"ui", L"切换位置固定 pinned=%d x=%d y=%d",
           (int)st.candidatePinned, st.candidatePinX, st.candidatePinY);
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
        if (wParam == 2)
        {
            // 词典操作反馈浮窗到期：只关浮窗本体，候选窗保持显示
            KillTimer(hwnd, 2);
            HideTip();
            return 0;
        }
        KillTimer(hwnd, (UINT_PTR)wParam);
        HideTip();
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

    case WM_MOUSEHOVER:
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

    case WM_MOUSELEAVE:
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

    case WM_LBUTTONDOWN:
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

    case WM_SETCURSOR:
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
        break;
    }

    case WM_MOUSEWHEEL:
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
        int row = (pt.y - yOff) / itemH; // 页内行号（0 起）

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

    case WM_RBUTTONUP:
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
            int row = (pt.y - yOff) / itemH;
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
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"置顶该词");
        AppendMenuW(menu, MF_STRING, 2, L"提升词频");
        AppendMenuW(menu, MF_STRING | (inDict ? 0 : MF_GRAYED), 3, L"降低词频");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (inDict ? 0 : MF_GRAYED), 4, L"从用户词典删除");
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
        if (cmd == 1 || cmd == 2 || cmd == 3 || cmd == 4)
        {
            CandDictOp op = (cmd == 1) ? CandDictOp::Pin
                        : (cmd == 2) ? CandDictOp::Boost
                        : (cmd == 3) ? CandDictOp::Lower : CandDictOp::Delete;
            // 失败仅记日志不打断输入（词典窗提供手动键兜底）；成功后下一次
            // 击键即见新排序（引擎经 g_reloadUserDict 已重载）。
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

    case WM_XBUTTONUP:
    {
        // 鼠标侧键翻页：后退键=上一页、前进键=下一页（与滚轮/‹›同夹取语义）
        int xb = GET_XBUTTON_WPARAM(wParam);
        if (xb == XBUTTON1) DoPageStep(-1);
        else if (xb == XBUTTON2) DoPageStep(1);
        return TRUE; // 已处理：不给宿主留重复导航语义
    }

    case WM_MBUTTONUP:
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
            int row = (pt.y - yOff) / itemH;
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

    case WM_DESTROY:
        if (_ttHwnd) { DestroyWindow(_ttHwnd); _ttHwnd = nullptr; } // UI 线程内安全销毁
        _hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace lingxi
