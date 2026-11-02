//
// 使用帮助窗（交互速查）。原 settingsui.cpp 帮助窗段拆出（P1 重构）。
// 交互功能越加越多（工具条/修饰键/中键/侧键…），可发现性靠集中速查页兜底。
// 只读多行 EDIT + 系统滚动，内容由 kHelpText 静态速查 + 打开时刻实时配置
// 快照拼成（BuildHelpText）。
//
#include "uiinternal.h"

#include <windows.h>
#include <windowsx.h>
#include <cwctype>
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
    L"· 右键符号：收藏/取消收藏（已收藏符号格右上角有小★）\r\n"
    L"· \"最近\"页自动记录最近使用的符号（最新在前，最多 12 个；右键\"最近\"标签清空）\r\n"
    L"· \"收藏\"页显示收藏的符号（最新在前，最多 60 个；右键\"收藏\"标签清空）\r\n"
    L"· \"日期\"页插入当前日期/时间/星期（打开/切页时现算）\r\n"
    L"· 分类：常用/数学/货币/箭头/表情/手势/动物/食物/活动/注音/希腊/框线/序号/颜文字\r\n"
    L"· 静态分类记忆上次查看页（重开直达）；滚轮换分类\r\n"
    L"· 关闭：右上角 × / 右键空白处 / 重按热键或\"符\"按钮\r\n"
    L"\r\n"
    L"【中英文切换】\r\n"
    L"· 切换键在设置页选择（Shift / Ctrl / CapsLock / 关闭）；无拼音时触发\r\n"
    L"· 中文模式：字母组拼音，标点自动全角\r\n"
    L"· U 模式拆字：u+部件读音打不会读的字（ujinjinjin→鑫、umingkong→曌；全拼限定，无命中自动落回拼音）\r\n"
    L"\r\n"
    L"【托盘图标】\r\n"
    L"· 左键：打开/关闭设置　　右键：菜单　　中键：使用帮助\r\n"
    L"· 右键菜单：拼音模式 / 暂停预测 / 暂停 30 分钟或 1 小时 / 打字统计 /\r\n"
    L"  无痕模式 / 固定候选窗位置 / 设置备份导出导入 / 重新加载用户词典 /\r\n"
    L"  打开数据文件夹 / 屏蔽词管理 / 剪贴板历史 / 恢复默认设置 / 开机自动启动 / 退出\r\n"
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
    L"· 帮助窗 Ctrl+F：查找速查内容（输入即跳转，Enter/F3=下一个，Esc=关闭查找）\r\n"
    L"· 帮助窗 Esc：关闭　　底部\"复制全部文本\"：速查内容一键进剪贴板\r\n"
    L"· 设置窗 Esc：隐藏；位置与尺寸自动记忆（重开恢复）\r\n"
    L"\r\n"
    L"【词典与学习】\r\n"
    L"· 候选词右键菜单可置顶/提升/降频/删除用户词条，下次击键即生效\r\n"
    L"· 无痕模式（托盘菜单）：开启后选词不学习、不计打字统计、不记最近符号与\r\n"
    L"  剪贴板历史；预测与收藏不受影响，关闭即恢复记录\r\n"
    L"· 词典窗顶部搜索框：词条/拼音实时过滤（不区分大小写）\r\n"
    L"· 屏蔽词管理窗（托盘菜单 / 词典窗\"屏蔽词\"按钮）：所列词永不进入候选，\r\n"
    L"  保存即热载生效（与用户词典同通道）；上限 500 词、每词 50 字符\r\n"
    L"· 管理窗列表双击：条目回填输入框（屏蔽词/自定义短语/快捷短语，改后重新添加）\r\n"
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
// Ctrl+F 查找行（批次 H）：底部按钮条上方的"查找: + 输入框 + 下一个"，
// 默认隐藏，开时压缩正文高度；输入即跳转（EN_UPDATE），Enter/按钮=下一个，
// 找不到回绕到开头再找一次，仍未找到按钮反馈"未找到"。
bool g_findVisible = false;
HWND g_hFindLabel = nullptr, g_hFindEdit = nullptr, g_hFindBtn = nullptr;
WNDPROC g_oldFindEditProc = nullptr;
constexpr int IDC_HELP_COPY = 350;
constexpr int IDC_HELP_FINDNEXT = 351;
constexpr int IDC_HELP_FINDEDIT = 352;
constexpr int kHelpBarH = 48; // 底部按钮条高（px，帮助窗无 DPI 缩放基础设施）
constexpr int kFindBarH = 34; // 查找行高

// 统一布局：正文让出底部按钮条（查找行可见时再让出查找行）。
// WM_SIZE 与查找行开/关共用，保证拉伸窗口后控件不漂移。
void HelpLayout()
{
    if (!g_hHelp) return;
    RECT rc;
    GetClientRect(g_hHelp, &rc);
    int cx = rc.right, cy = rc.bottom;
    int findH = g_findVisible ? kFindBarH : 0;
    HWND edit = (HWND)GetWindowLongPtrW(g_hHelp, GWLP_USERDATA);
    if (edit)
        MoveWindow(edit, 0, 0, cx, cy > kHelpBarH + findH ? cy - kHelpBarH - findH : cy, TRUE);
    if (g_hHelpBtn)
        MoveWindow(g_hHelpBtn, 12, cy > kHelpBarH ? cy - kHelpBarH + 8 : 0, 140, 32, TRUE);
    if (g_findVisible)
    {
        int fy = cy - kHelpBarH - kFindBarH + 4;
        if (g_hFindLabel) MoveWindow(g_hFindLabel, 10, fy + 5, 40, 20, TRUE);
        if (g_hFindEdit) MoveWindow(g_hFindEdit, 54, fy, cx - 54 - 96, 24, TRUE);
        if (g_hFindBtn) MoveWindow(g_hFindBtn, cx - 90, fy, 80, 24, TRUE);
    }
}

// 查找：从 from 起（字符索引）大小写不敏感找第一处，命中选中+滚到可见；
// 尾后未中回绕到开头再找；仍未中按钮反馈"未找到"（下次输入自动复位）。
void HelpFind(HWND help, int from)
{
    HWND edit = (HWND)GetWindowLongPtrW(help, GWLP_USERDATA);
    if (!edit || !g_hFindEdit) return;
    int flen = GetWindowTextLengthW(g_hFindEdit);
    if (flen <= 0) return;
    std::wstring needle(flen + 1, L'\0');
    GetWindowTextW(g_hFindEdit, &needle[0], flen + 1);
    needle.resize(flen);
    int tlen = GetWindowTextLengthW(edit);
    std::wstring text(tlen + 1, L'\0');
    GetWindowTextW(edit, &text[0], tlen + 1);
    text.resize(tlen);
    for (auto& ch : needle) ch = (wchar_t)towlower(ch);
    for (auto& ch : text) ch = (wchar_t)towlower(ch);
    size_t pos = text.find(needle, from > 0 ? (size_t)from : 0);
    if (pos == std::wstring::npos && from > 0) pos = text.find(needle, 0); // 回绕
    if (g_hFindBtn) SetWindowTextW(g_hFindBtn, pos == std::wstring::npos ? L"未找到" : L"下一个");
    if (pos != std::wstring::npos)
    {
        SendMessageW(edit, EM_SETSEL, (WPARAM)pos, (LPARAM)(pos + needle.size()));
        SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    }
}

void HelpFindNext(HWND help) // Enter/"下一个"：从当前选中末尾继续
{
    HWND edit = (HWND)GetWindowLongPtrW(help, GWLP_USERDATA);
    if (!edit) return;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    HelpFind(help, (int)s1);
}

void CloseFindBar(HWND help)
{
    g_findVisible = false;
    if (g_hFindLabel) ShowWindow(g_hFindLabel, SW_HIDE);
    if (g_hFindEdit) ShowWindow(g_hFindEdit, SW_HIDE);
    if (g_hFindBtn) ShowWindow(g_hFindBtn, SW_HIDE);
    HelpLayout();
    HWND edit = (HWND)GetWindowLongPtrW(help, GWLP_USERDATA);
    if (edit) SetFocus(edit);
}

void OpenFindBar(HWND help)
{
    g_findVisible = true;
    if (g_hFindLabel) ShowWindow(g_hFindLabel, SW_SHOW);
    if (g_hFindBtn) ShowWindow(g_hFindBtn, SW_SHOW);
    if (g_hFindEdit) ShowWindow(g_hFindEdit, SW_SHOW);
    // 预填当前选中片段（无选中则清空）：选中即所查是常态
    HWND edit = (HWND)GetWindowLongPtrW(help, GWLP_USERDATA);
    if (edit && g_hFindEdit)
    {
        DWORD s0 = 0, s1 = 0;
        SendMessageW(edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
        if (s1 > s0 && s1 <= (DWORD)GetWindowTextLengthW(edit))
        {
            int tlen = GetWindowTextLengthW(edit);
            std::wstring text(tlen + 1, L'\0');
            GetWindowTextW(edit, &text[0], tlen + 1);
            text.resize(tlen);
            SetWindowTextW(g_hFindEdit, text.substr(s0, s1 - s0).c_str());
        }
        else SetWindowTextW(g_hFindEdit, L"");
    }
    HelpLayout();
    if (g_hFindEdit)
    {
        SetFocus(g_hFindEdit);
        SendMessageW(g_hFindEdit, EM_SETSEL, 0, -1); // 全选备改
    }
}

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
    if (msg == WM_KEYDOWN && wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (g_findVisible && g_hFindEdit) // 已开：回到查找框全选备改
        {
            SetFocus(g_hFindEdit);
            SendMessageW(g_hFindEdit, EM_SETSEL, 0, -1);
        }
        else OpenFindBar(GetParent(h));
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_F3) // F3=下一个（未开查找行则先开，Windows 惯例）
    {
        if (g_findVisible) HelpFindNext(GetParent(h));
        else OpenFindBar(GetParent(h));
        return 0;
    }
    return CallWindowProcW(g_oldHelpEditProc, h, msg, wp, lp);
}

// 查找框子类化：Enter=下一个、Esc=关查找行（不关窗）、Ctrl+F=全选备改
LRESULT CALLBACK HelpFindEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN)
    {
        if (wp == VK_ESCAPE) { CloseFindBar(GetParent(h)); return 0; }
        if (wp == VK_RETURN) { HelpFindNext(GetParent(h)); return 0; }
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    return CallWindowProcW(g_oldFindEditProc, h, msg, wp, lp);
}

LRESULT CALLBACK HelpWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE)
    {
        HelpLayout(); // 正文/按钮条/查找行统一布局（拉伸后不漂移）
        return 0;
    }
    if (msg == WM_MOUSEWHEEL)
    {
        // Ctrl+滚轮调帮助正文字号（12..28；速查页长文可读性，浏览器缩放手势同款）。
        // 审计轮 10 修复：原实现不检查 Ctrl——焦点在按钮等处（滚轮消息落到
        // 顶层窗）时裸滚轮误改字号，与帮助文本"Ctrl + 滚轮"条款不符。
        if (GetKeyState(VK_CONTROL) & 0x8000)
        {
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
        }
        else
        {
            // 非缩放手势：滚轮转发正文（焦点在按钮等处时也能正常滚动文本）
            HWND edit = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (edit) SendMessageW(edit, WM_MOUSEWHEEL, wp, lp);
        }
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_HELP_COPY)
        {
            HelpCopyAll(h);
            return 0;
        }
        if (id == IDC_HELP_FINDNEXT && code == BN_CLICKED)
        {
            HelpFindNext(h);
            return 0;
        }
        if (id == IDC_HELP_FINDEDIT && code == EN_UPDATE) // 输入即跳转（从当前选中首起）
        {
            HWND edit = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (edit)
            {
                DWORD s0 = 0, s1 = 0;
                SendMessageW(edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
                HelpFind(h, (int)s0);
            }
            return 0;
        }
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
        g_hFindLabel = nullptr;
        g_hFindEdit = nullptr;
        g_hFindBtn = nullptr;
        g_oldFindEditProc = nullptr;
        g_findVisible = false;
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
    // 查找行（Ctrl+F 呼出，默认隐藏；位置由 HelpLayout 统一计算）
    g_hFindLabel = CreateWindowExW(0, L"STATIC", L"查找:", WS_CHILD,
                                   10, 0, 40, 20, g_hHelp, NULL, hInst, NULL);
    g_hFindEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                                  54, 0, W - 54 - 96, 24, g_hHelp,
                                  (HMENU)(INT_PTR)IDC_HELP_FINDEDIT, hInst, NULL);
    g_hFindBtn = CreateWindowExW(0, L"BUTTON", L"下一个",
                                 WS_CHILD | WS_TABSTOP,
                                 0, 0, 80, 24, g_hHelp,
                                 (HMENU)(INT_PTR)IDC_HELP_FINDNEXT, hInst, NULL);
    g_oldFindEditProc =
        (WNDPROC)SetWindowLongPtrW(g_hFindEdit, GWLP_WNDPROC, (LONG_PTR)HelpFindEditProc);
    SendMessageW(g_hFindLabel, WM_SETFONT, (WPARAM)g_helpFont, TRUE);
    SendMessageW(g_hFindEdit, WM_SETFONT, (WPARAM)g_helpFont, TRUE);
    SendMessageW(g_hFindBtn, WM_SETFONT, (WPARAM)g_helpFont, TRUE);
    HelpLayout(); // 建齐后统一摆位（含隐藏态正文全高）
    UpdateWindow(g_hHelp);
}

} // namespace lingxi
