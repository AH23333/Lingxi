//
// 使用帮助窗（交互速查）。原 settingsui.cpp 帮助窗段拆出（P1 重构）。
// 交互功能越加越多（工具条/修饰键/中键/侧键…），可发现性靠集中速查页兜底。
// 只读多行 EDIT + 系统滚动，内容由 kHelpText 静态速查 + 打开时刻实时配置
// 快照拼成（BuildHelpText）。
//
#include "uiinternal.h"

#include <windows.h>
#include <windowsx.h>
#include <string>

#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"
#include "usagestats.h"

namespace lingxi {

namespace {

const wchar_t* kHelpText =
    L"灵犀输入法 · 交互速查\r\n"
    L"\r\n"
    L"【候选窗】\r\n"
    L"· 数字键 1-9 / 鼠标点击：选词上屏\r\n"
    L"· 滚轮、‹ › 按钮、鼠标侧键（后退/前进）：翻页\r\n"
    L"· Ctrl + 滚轮：候选字号调节（12-32，即时生效）\r\n"
    L"· Shift + 滚轮：候选窗不透明度调节（70-100%）\r\n"
    L"· 按住拼音回显行或底部工具条空白：拖动窗口，松手即固定位置\r\n"
    L"· 右键候选词：词典菜单（置顶/提升/降频/删除/屏蔽/复制/联网搜索/查拼音），成功有浮窗反馈\r\n"
    L"· 中键点击候选词：置顶　Shift+中键：降频　Ctrl+中键：删除\r\n"
    L"· Ctrl + 左键候选词：联网搜索　　Shift + 左键：复制该词\r\n"
    L"· Ctrl + Shift + 左键候选词：从用户词典删除\r\n"
    L"· 右键拼音回显行：复制拼音串\r\n"
    L"· 工具条按钮：固=固定位置开关（右键解除）　符=符号面板　设=设置（右键换肤）\r\n"
    L"· 右键工具条空白：7 款皮肤直选子菜单\r\n"
    L"· 悬停按钮或被截断的长候选：显示提示\r\n"
    L"\r\n"
    L"【符号面板】（托盘菜单 / Ctrl+Shift+B（设置页可开） / 候选窗\"符\"按钮）\r\n"
    L"· 点击符号插入，面板保留便于连续输入；悬停高亮+手型光标\r\n"
    L"· \"最近\"页自动记录最近使用的符号（最新在前，最多 12 个；右键\"最近\"标签清空）\r\n"
    L"· \"日期\"页插入当前日期/时间/星期（打开/切页时现算）\r\n"
    L"· 分类：常用/数学/货币/箭头/表情/手势/动物/食物/活动/注音/希腊/框线/序号/颜文字\r\n"
    L"· 静态分类记忆上次查看页（重开直达）；滚轮换分类\r\n"
    L"· 关闭：右上角 × / 右键 / 重按热键或\"符\"按钮\r\n"
    L"\r\n"
    L"【中英文切换】\r\n"
    L"· 切换键在设置页选择（Shift / Ctrl / CapsLock / 关闭）；无拼音时触发\r\n"
    L"· 中文模式：字母组拼音，标点自动全角\r\n"
    L"\r\n"
    L"【托盘图标】\r\n"
    L"· 左键：打开/关闭设置　　右键：菜单　　中键：使用帮助\r\n"
    L"· 右键菜单：拼音模式 / 暂停预测 / 暂停 30 分钟或 1 小时 / 打字统计 /\r\n"
    L"  固定候选窗位置 / 设置备份导出导入 / 重新加载用户词典 / 打开数据文件夹 /\r\n"
    L"  屏蔽词管理 / 剪贴板历史 / 恢复默认设置 / 开机自动启动 / 退出\r\n"
    L"· Ctrl+Alt+P：全局热键切换暂停预测\r\n"
    L"· Ctrl+Alt+H：全局热键打开使用帮助\r\n"
    L"· Ctrl+Alt+V：全局热键打开剪贴板历史\r\n"
    L"· 悬停图标显示状态摘要（暂停/拼音关/双拼/繁体）\r\n"
    L"\r\n"
    L"【打字统计】（托盘菜单 / 设置·词典页）\r\n"
    L"· 今日/累计选词次数、上屏汉字数、活跃与连续天数\r\n"
    L"· 刷新实时更新；清零不可恢复；\"导出诊断报告\"生成可分享的文本文件\r\n"
    L"· Esc 关闭统计窗\r\n"
    L"\r\n"
    L"【剪贴板历史】（托盘菜单 / Ctrl+Alt+V）\r\n"
    L"· 全局复制的文本自动入史：上限 50 条、单条 2000 字符、重复复制前移不重记\r\n"
    L"· 双击条目或\"复制选中\"写回剪贴板；删除/清空即时落盘\r\n"
    L"· 列表下方实时显示选中条目全文；Esc 关闭\r\n"
    L"\r\n"
    L"【帮助窗 / 设置窗】\r\n"
    L"· 帮助窗 Ctrl + 滚轮：正文字号调节（12-28）\r\n"
    L"· 帮助窗 Esc：关闭　　底部\"复制全部文本\"：速查内容一键进剪贴板\r\n"
    L"· 设置窗 Esc：隐藏；位置与尺寸自动记忆（重开恢复）\r\n"
    L"\r\n"
    L"【词典与学习】\r\n"
    L"· 候选词右键菜单可置顶/提升/降频/删除用户词条，下次击键即生效\r\n"
    L"· 词典窗顶部搜索框：词条/拼音实时过滤（不区分大小写）\r\n"
    L"· 屏蔽词管理窗（托盘菜单 / 词典窗\"屏蔽词\"按钮）：所列词永不进入候选，\r\n"
    L"  保存即热载生效（与用户词典同通道）；上限 500 词、每词 50 字符\r\n"
    L"· 自定义短语（设置·词典页）：缩写 2-16 位字母，整串打出即置顶（如 addr→地址）\r\n"
    L"· 快捷短语（设置·词典页）：自定义热键一键输入句式（须含 Ctrl 或 Alt）\r\n"
    L"· 管理窗 Esc：随时关闭（词典/自定义短语/快捷短语/屏蔽词/统计/剪贴板历史，窗口隐藏复用）\r\n"
    L"\r\n"
    L"设置即时保存（HKCU\\Software\\LingxiIme），全部改动立即生效。\r\n"
    L"命令行：--version / --help / --diagnostics / --settings / --symbols。\r\n";

// 帮助文本 = 静态速查 + 打开时刻的实时配置快照（只读，改设置请进设置页/托盘）
std::wstring BuildHelpText()
{
    Settings s = LoadSettings();
    static const wchar_t* kSkinNames[] = { L"翠绿", L"青碧", L"橙", L"红", L"金", L"茶棕", L"玫粉" };
    static const wchar_t* kToggleNames[] = { L"关闭", L"Shift", L"Shift 或 Ctrl",
                                             L"CapsLock", L"仅 Ctrl" };
    static const wchar_t* kDarkNames[] = { L"跟随系统", L"浅色", L"深色" };
    wchar_t cfg[4096];
    _snwprintf_s(cfg, _TRUNCATE,
        L"%s"
        L"【当前生效配置】（打开时刻快照）\r\n"
        L"· 中英切换键：%ls（无拼音时触发）\r\n"
        L"· 皮肤：%ls　候选数：%u　字号：%u　不透明度：%u%%\r\n"
        L"· 界面字体：%ls　明暗：%ls\r\n"
        L"· 模糊音：%ls　双拼：%ls　繁体输出：%ls\r\n"
        L"· 英文补全：%ls　中文标点：%ls　选词联想：%ls\r\n"
        L"· 符号热键 Ctrl+Shift+B：%ls　LLM 重排：%ls\r\n"
        L"· 位置固定：%ls　开机自启动：%ls　预测：%ls\r\n"
        L"· 打字：%ls\r\n"
        L"\r\n"
        L"（本页只读；修改请进设置页或托盘菜单，改动即时生效）\r\n",
        kHelpText,
        kToggleNames[s.toggleMode],
        kSkinNames[s.skinIndex], s.candidateCount, s.candidateFontSize,
        s.candidateOpacity,
        kUiFontNames[s.uiFont], kDarkNames[s.darkMode],
        s.fuzzyPinyin ? L"开" : L"关", s.shuangpin ? L"小鹤" : L"关",
        s.traditionalOutput ? L"开" : L"关",
        s.enCompletion ? L"开" : L"关", s.cnPunct ? L"开" : L"关",
        s.assocAfterPick ? L"开" : L"关",
        s.symbolHotkey ? L"开" : L"关", s.llmRerank ? L"开" : L"关",
        s.candidatePinned ? L"开" : L"关",
        IsAutoStartEnabled() ? L"开" : L"关",
        s.paused ? L"已暂停" : L"运行中",
        BuildStatsBrief().c_str());
    return cfg;
}

HWND g_hHelp = nullptr;
int g_helpFontH = 15;  // 帮助正文字号（Ctrl+滚轮 12..28 可调）
HFONT g_helpFont = nullptr; // 进程内复用，防反复开关帮助窗泄漏
HWND g_hHelpBtn = nullptr;  // 底部"复制全部文本"按钮
WNDPROC g_oldHelpEditProc = nullptr; // 正文 EDIT 子类化（Esc 关窗）
constexpr int IDC_HELP_COPY = 350;
constexpr int kHelpBarH = 48; // 底部按钮条高（px，帮助窗无 DPI 缩放基础设施）

// 复制帮助正文到剪贴板（含实时配置快照；剪贴板被占 3×30ms 重试——
// 复制是用户主动点击，偶发占用不该静默失败）
void HelpCopyAll(HWND h)
{
    HWND edit = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!edit) return;
    int len = GetWindowTextLengthW(edit);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (size_t)(len + 1) * sizeof(wchar_t));
    bool ok = false;
    if (hg)
    {
        wchar_t* p = (wchar_t*)GlobalLock(hg);
        if (p)
        {
            GetWindowTextW(edit, p, len + 1);
            GlobalUnlock(hg);
            bool opened = false;
            for (int i = 0; i < 3 && !(opened = OpenClipboard(h)); i++) Sleep(30);
            if (opened)
            {
                if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, hg))
                {
                    ok = true;
                    hg = NULL; // 成功后内存归系统所有
                }
                CloseClipboard();
            }
        }
    }
    if (hg) GlobalFree(hg);
    if (g_hHelpBtn) SetWindowTextW(g_hHelpBtn, ok ? L"已复制" : L"复制失败");
    SetTimer(h, 3, 1500, NULL); // 1.5s 后恢复按钮文字
    LX_LOG(L"ui", L"帮助文本复制 ok=%d len=%d", (int)ok, len);
}

LRESULT CALLBACK HelpEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        DestroyWindow(GetParent(h)); // 焦点在正文时 Esc 同样关窗
        return 0;
    }
    return CallWindowProcW(g_oldHelpEditProc, h, msg, wp, lp);
}

LRESULT CALLBACK HelpWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE)
    {
        HWND edit = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
        int cx = LOWORD(lp), cy = HIWORD(lp);
        int bodyH = cy > kHelpBarH ? cy - kHelpBarH : cy; // 正文让出底部按钮条
        if (edit) MoveWindow(edit, 0, 0, cx, bodyH, TRUE);
        if (g_hHelpBtn) MoveWindow(g_hHelpBtn, 12, cy > kHelpBarH ? cy - kHelpBarH + 8 : 0,
                                   140, 32, TRUE);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL)
    {
        // Ctrl+滚轮调帮助正文字号（12..28；速查页长文可读性，浏览器缩放手势同款）
        int nh = g_helpFontH + ((short)GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
        if (nh < 12) nh = 12;
        if (nh > 28) nh = 28;
        if (nh != g_helpFontH)
        {
            g_helpFontH = nh;
            if (g_helpFont) DeleteObject(g_helpFont);
            g_helpFont = CreateFontW(-g_helpFontH, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                     DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                     L"Microsoft YaHei UI");
            HWND edit = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (edit) SendMessageW(edit, WM_SETFONT, (WPARAM)g_helpFont, TRUE);
            LX_LOG(L"ui", L"帮助窗字号调整为 %d", g_helpFontH);
        }
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDC_HELP_COPY)
    {
        HelpCopyAll(h);
        return 0;
    }
    if (msg == WM_TIMER && wp == 3)
    {
        KillTimer(h, 3);
        if (g_hHelpBtn) SetWindowTextW(g_hHelpBtn, L"复制全部文本"); // 反馈复位
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        DestroyWindow(h); // 帮助窗本身持焦点时 Esc 关窗
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC) // 只读 EDIT 默认灰字 → 正常深色可读
    {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        SetTextColor(hdc, RGB(32, 32, 32));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    if (msg == WM_DESTROY)
    {
        g_hHelp = nullptr;
        g_hHelpBtn = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void ShowHelpWindow(HINSTANCE hInst)
{
    if (g_hHelp && IsWindow(g_hHelp))
    {
        ShowWindow(g_hHelp, SW_RESTORE);
        SetForegroundWindow(g_hHelp);
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = HelpWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LingxiHelp";
    RegisterClassW(&wc);

    const int W = 560, H = 640;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT wa{ 0, 0, 1920, 1040 };
    if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - H) / 2;

    g_hHelp = CreateWindowExW(WS_EX_TOPMOST, L"LingxiHelp", L"灵犀输入法 · 使用帮助",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              x, y, W, H, NULL, NULL, hInst, NULL);
    if (!g_hHelp) return;

    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                    ES_AUTOVSCROLL | WS_VSCROLL,
                                0, 0, W, H - kHelpBarH, g_hHelp, NULL, hInst, NULL);
    SetWindowLongPtrW(g_hHelp, GWLP_USERDATA, (LONG_PTR)edit);
    // 正文 EDIT 子类化：焦点在正文时 Esc 关窗（帮助窗其余区域由 HelpWndProc 处理）
    g_oldHelpEditProc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)HelpEditProc);
    if (!g_helpFont) // 进程内复用；Ctrl+滚轮调号时在 HelpWndProc 重建
        g_helpFont = CreateFontW(-g_helpFontH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    SendMessageW(edit, WM_SETFONT, (WPARAM)g_helpFont, TRUE);
    SetWindowTextW(edit, BuildHelpText().c_str());
    SendMessageW(edit, EM_SETSEL, 0, 0); // 定位开头
    // 底部按钮条："复制全部文本"（速查页分享/存档一键进剪贴板）
    g_hHelpBtn = CreateWindowExW(0, L"BUTTON", L"复制全部文本",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 12, H - kHelpBarH + 8, 140, 32, g_hHelp,
                                 (HMENU)(INT_PTR)IDC_HELP_COPY, hInst, NULL);
    SendMessageW(g_hHelpBtn, WM_SETFONT, (WPARAM)g_helpFont, TRUE);
    UpdateWindow(g_hHelp);
}

} // namespace lingxi
