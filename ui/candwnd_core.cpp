#include "window.h"

#include "candwnd_internal.h"

#include "../common/proto.h"
#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

//
// 候选窗核心（P2 拆分自 window.cpp，2026-10-31）：窗口生命周期（Init/
// Shutdown/Show/Hide）、状态与翻页、固钉、toast、布局定位、消息分发
// （WndProc 壳）。绘制见 candwnd_render.cpp；鼠标手势/工具条/菜单见
// candwnd_interact.cpp。
//

namespace lingxi {

static const wchar_t kCandWndClass[] = L"LingxiCandidateWnd";

// H4：跨线程窗口状态变更封包（WM_APP+30：避开托盘 +10 / 通知 +20/+21）。
// 非属主线程调用 Show/Hide/SetPage/ShowModeToast/ShowTextToast 一律经此投递
// UI 线程串行执行——SetWindowPos/KillTimer/SetTimer 全部回到属主线程，
// 定时器创建/销毁线程一致性成立；重排封包（kCmdRerankShow）另做 ver 陈旧校验
constexpr UINT WM_APP_CANDCMD = WM_APP + 30;
enum CandCmdKind
{
    kCmdShow = 0,      // 预测线程新候选：无条件应用（单线程串行，天然有序）
    kCmdRerankShow,    // 重排线程同 ver 刷新：ver 已变则丢弃
    kCmdHide,
    kCmdPage,
    kCmdToast,
    kCmdTextToast,
};
struct ShowReq
{
    RECT caret{};
    std::vector<std::wstring> cands;
    std::vector<std::wstring> anno;
    std::wstring composing;
    std::wstring text;      // kCmdTextToast 气泡文字
    bool dark = false;
    bool english = false;   // kCmdToast 中/英标志
    uint32_t ver = 0;
    uint32_t pageStart = 0; // kCmdPage
};

bool CandWindow::Init(HINSTANCE hInst)
{
    InitializeCriticalSection(&_cs);
    _csInit = true;
    _ownerTid = GetCurrentThreadId(); // H4：窗口属主线程（消息循环线程）

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
    std::vector<std::wstring> pageAnno;
    std::wstring composing;
    {
        EnterCriticalSection(&_cs);
        size_t pgBegin = 0, pgEnd = 0;
        CurrentPageRangeLocked(&pgBegin, &pgEnd);
        page.reserve(pgEnd - pgBegin);
        for (size_t i = pgBegin; i < pgEnd; i++) page.push_back(_cands[i]);
        for (size_t i = pgBegin; i < pgEnd; i++)
            pageAnno.push_back(i < _anno.size() ? _anno[i] : std::wstring());
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
        for (size_t i = 0; i < page.size(); i++)
        {
            std::wstring probe = page[i];
            if (i < pageAnno.size() && !pageAnno[i].empty()) probe += L"（" + pageAnno[i] + L"）"; // 注音占宽
            GetTextExtentPoint32W(hdc, probe.c_str(), (int)probe.size(), &sz);
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
                      bool dark, uint32_t ver, const std::wstring& composing,
                      const std::vector<std::wstring>& anno)
{
    if (!_hwnd) return;
    if (GetCurrentThreadId() != _ownerTid)
    {   // H4：预测线程等非属主线程 → 封包投递 UI 线程（新 ver 无条件应用）
        ShowReq* req = new ShowReq;
        req->caret = caretScreen; req->cands = cands; req->anno = anno;
        req->composing = composing; req->dark = dark; req->ver = ver;
        PostCmd(kCmdShow, req);
        return;
    }
    if (uint32_t fs = LoadSettings().candidateFontSize; fs != _fontSize)
        RebuildFonts(fs); // 字号设置变化即时生效
    else if (LoadSettings().uiFont != _fontFace)
        RebuildFonts(_fontSize); // 字体族设置变化即时生效（字号不变只换 face）
    if (uint32_t sk = LoadSettings().skinIndex; sk != _skinIndex)
        ApplySkin(sk); // 皮肤设置变化即时生效
    EnterCriticalSection(&_cs);
    _caret = caretScreen;
    _cands = cands;
    _anno = anno;
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
    _toastText.clear();
    LeaveCriticalSection(&_cs);
    KillTimer(_hwnd, 1);
    HideTip(); // 候选帧接管窗口（跨线程：tooltip 仅 ShowWindow 隐藏，安全）
    LayoutAndPosition();
    _visible = true;
}

void CandWindow::SetPage(uint32_t ver, uint32_t pageStart)
{
    if (!_hwnd) return;
    if (GetCurrentThreadId() != _ownerTid)
    {   // H4：客户端读线程（MSG_PAGE）→ 封包投递 UI 线程
        ShowReq* req = new ShowReq;
        req->ver = ver; req->pageStart = pageStart;
        PostCmd(kCmdPage, req);
        return;
    }
    bool changed = false;
    EnterCriticalSection(&_cs);
    // 与 Show() 同因：翻页指令期间 toast 态必须让位（清态+杀定时器）
    _toastMode = false;
    _toastText.clear();
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
    if (GetCurrentThreadId() != _ownerTid)
    {   // H4：客户端读线程（MSG_MODE_TOAST）→ 封包投递 UI 线程（SetTimer 归属主）
        ShowReq* req = new ShowReq;
        req->english = english;
        PostCmd(kCmdToast, req);
        return;
    }
    bool dark = _dark;
    EnterCriticalSection(&_cs);
    _toastMode = true;
    _toastEnglish = english;
    _toastText.clear(); // 文字气泡残留会让本提示画成上次的文字
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
    _visible = true; // toast 也在显示：1s 到期完整隐藏路径依赖此标记
                     // （不置位则纯 toast 场景到期不隐藏、MSG_HIDE 守卫拦不住）
    HideTip();
    SetTimer(_hwnd, 1, 1000, NULL);
}

// 通用文字气泡（R8 每日目标达成提醒）：与 ShowModeToast 同态机——
// WM_TIMER(1) 到期隐藏、Show()/SetPage() 候选接管清态、_visible 置位；
// 差异仅两点：宽度随文字自适应（_toastFont 实测），驻留 1.6s。
void CandWindow::ShowTextToast(const wchar_t* text)
{
    if (!_hwnd || !text || !*text) return;
    if (GetCurrentThreadId() != _ownerTid)
    {   // H4：ApplyLearn 可能跑在客户端读线程 → 封包投递 UI 线程
        ShowReq* req = new ShowReq;
        req->text = text;
        PostCmd(kCmdTextToast, req);
        return;
    }
    std::wstring t = text;
    int w = 78;
    {
        HDC hdc = GetDC(_hwnd);
        if (hdc)
        {
            HGDIOBJ old = SelectObject(hdc, _toastFont);
            SIZE sz{};
            if (GetTextExtentPoint32W(hdc, t.c_str(), (int)t.size(), &sz) && sz.cx > 0)
                w = sz.cx + 28;
            SelectObject(hdc, old);
            ReleaseDC(_hwnd, hdc);
        }
    }
    int h = 58;
    EnterCriticalSection(&_cs);
    _toastMode = true;
    _toastText = std::move(t);
    LeaveCriticalSection(&_cs);
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
    _visible = true; // 同 ShowModeToast：到期隐藏路径依赖此标记
    HideTip();
    SetTimer(_hwnd, 1, 1600, NULL);
}

void CandWindow::Hide()
{
    if (!_hwnd || !_visible) return;
    if (GetCurrentThreadId() != _ownerTid)
    {   // H4：客户端读线程/预测线程（断连清理、MSG_HIDE、空文本）→ 封包投递
        PostCmd(kCmdHide, nullptr);
        return;
    }
    _visible = false;
    HideTip();
    ShowWindow(_hwnd, SW_HIDE);
    EnterCriticalSection(&_cs);
    _cands.clear();
    _composing.clear();
    _pageStart = 0;
    LeaveCriticalSection(&_cs);
}

// H4：跨线程显示请求——detached 重排线程不再直调 Show（与主线程鼠标/绘制
// 竞态窗口大）；封包投递到 UI 线程串行处理。投递失败（窗口已亡/退出期队列
// 满）就地删除封包——调用线程绝不触窗口。
void CandWindow::PostShow(const RECT& caretScreen, const std::vector<std::wstring>& cands,
                          bool dark, uint32_t ver, const std::wstring& composing,
                          const std::vector<std::wstring>& anno)
{
    if (!_hwnd) return;
    ShowReq* req = new ShowReq;
    req->caret = caretScreen;
    req->cands = cands;
    req->anno = anno;
    req->composing = composing;
    req->dark = dark;
    req->ver = ver;
    PostCmd(kCmdRerankShow, req); // 重排语义：UI 线程落地时 ver 已变则丢弃
}

// H4：状态变更封包投递——PostMessage 失败（窗口已亡/队列满）就地释放封包
void CandWindow::PostCmd(int kind, ShowReq* req)
{
    if (!PostMessageW(_hwnd, WM_APP_CANDCMD, (WPARAM)kind, (LPARAM)req))
        delete req;
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
        // 陈旧定时器守卫（"词框偶发不出现"根因之一）：toast 1s 到期瞬间用户开
        // 始打字，Show() 已清 _toastMode + KillTimer，但 WM_TIMER 已投递到本
        // 线程队列、KillTimer 撤不掉已投递消息——随后派发时若不查 _toastMode
        // 会把刚显示的候选完整隐藏（"切换中英后整 1s 打字，头一两键无候选"）。
        // _toastMode 仍真=真 toast 到期走隐藏；已假=候选帧已接管，直接返回。
        {
            EnterCriticalSection(&_cs);
            bool wasToast = _toastMode;
            _toastMode = false;
            _toastText.clear(); // 文字气泡到期同样清内容（防残留进下次模式气泡）
            LeaveCriticalSection(&_cs);
            if (!wasToast) return 0;
        }
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
        return OnMouseMove(hwnd, lParam);

    case WM_MOUSEHOVER:
        return OnMouseHover(hwnd, lParam);

    case WM_MOUSELEAVE:
        return OnMouseLeave(hwnd);

    case WM_LBUTTONDOWN:
        return OnLButtonDown(hwnd, lParam);

    case WM_SETCURSOR:
        // 手势细节在 candwnd_interact.cpp；未处理（非客户区）落 DefWindowProc
        if (OnSetCursor(hwnd, lParam)) return TRUE;
        break;

    case WM_MOUSEWHEEL:
        return OnMouseWheel(wParam);

    case WM_LBUTTONUP:
        return OnLButtonUp(hwnd, lParam);

    case WM_RBUTTONUP:
        return OnRButtonUp(hwnd, lParam);

    case WM_XBUTTONUP:
        return OnXButtonUp(wParam);

    case WM_MBUTTONUP:
        return OnMButtonUp(hwnd, lParam);

    case WM_APP_CANDCMD:
    {
        // H4：跨线程状态变更封包在 UI 线程串行落地（本线程调 Show/Hide/…
        // 走属主直执行路径，不再投递）。重排封包做陈旧校验——ver 已被更新
        // 预测覆盖则丢弃（防旧重排盖掉新候选；校验与应用间 µs 窗口仍可被
        // 新预测覆盖，后者随下一键自愈，远优于原先直调的整段竞态）
        ShowReq* req = (ShowReq*)lParam;
        switch ((int)wParam)
        {
        case kCmdShow:
            Show(req->caret, req->cands, req->dark, req->ver, req->composing, req->anno);
            break;
        case kCmdRerankShow:
        {
            uint32_t cur = 0;
            EnterCriticalSection(&_cs);
            cur = _ver;
            LeaveCriticalSection(&_cs);
            if (req->ver == cur)
                Show(req->caret, req->cands, req->dark, req->ver, req->composing, req->anno);
            break;
        }
        case kCmdHide:
            Hide();
            break;
        case kCmdPage:
            SetPage(req->ver, req->pageStart);
            break;
        case kCmdToast:
            ShowModeToast(req->english);
            break;
        case kCmdTextToast:
            ShowTextToast(req->text.c_str());
            break;
        }
        delete req;
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
