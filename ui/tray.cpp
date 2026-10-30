//
// 托盘图标 + 隐藏主窗口 + 全局热键 + 开机自启动。
// 原 settingsui.cpp 托盘段拆出（P1 重构）。
// - 隐藏消息窗口（LingxiUIMainWnd）承载托盘回调/热键/定时器；
//   explorer 重启自动恢复图标（TaskbarCreated）。
// - 左键：开/关设置窗；右键：菜单；中键：使用帮助。
// - Ctrl+Alt+P：切换暂停预测；Ctrl+Alt+H：打开使用帮助。
//
#include "settingsui.h"
#include "uiinternal.h"

#include <windows.h>
#include <shellapi.h>
#include <string>

#include "../common/log.h"
#include "../common/settings.h"
#include "usagestats.h"

namespace lingxi {

namespace {

constexpr wchar_t kMainWndClass[] = L"LingxiUIMainWnd";
constexpr UINT WM_APP_TRAY = WM_APP + 10; // 托盘回调消息
constexpr int kTrayHotkeyPause = 3; // RegisterHotKey 热键 ID（Ctrl+Alt+P 切暂停；与 main.cpp 侧 ID 不同窗不冲突）
constexpr int kTrayHotkeyHelp = 4;  // Ctrl+Alt+H 打开使用帮助

HWND g_hMain = nullptr;
UINT g_taskbarCreatedMsg = 0;
bool g_trayAdded = false;

// ---------------- 开机自启动（HKCU\Run）----------------
// 状态以 HKCU\Run 的 LingxiIme 键为准（不另存设置，避免双源漂移）。

void SetAutoStartEnabled(bool enable)
{
    const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY hk = nullptr;
    if (enable)
    {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &hk, NULL) != ERROR_SUCCESS)
            return;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t val[MAX_PATH + 4] = {};
        _snwprintf_s(val, _TRUNCATE, L"\"%s\"", exe);
        RegSetValueExW(hk, L"LingxiIme", 0, REG_SZ,
                       (const BYTE*)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        LX_LOG(L"ui", L"开机自启动已开启: %ls", val);
    }
    else
    {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &hk)
            != ERROR_SUCCESS)
            return;
        RegDeleteValueW(hk, L"LingxiIme");
        LX_LOG(L"ui", L"开机自启动已关闭");
    }
    if (hk) RegCloseKey(hk);
}

// 托盘悬停文字：基础名 + 状态摘要（暂停/拼音关/双拼/繁体——一眼可辨运行态）
std::wstring BuildTrayTip()
{
    Settings s = LoadSettings();
    std::wstring t = L"灵犀输入法";
    if (s.paused) t += L"（已暂停）";
    if (!s.pinyinEnabled) t += L" · 拼音关";
    if (s.shuangpin) t += L" · 双拼";
    if (s.traditionalOutput) t += L" · 繁体";
    return t;
}

void AddTrayIcon()
{
    if (!g_hMain || !g_hIcon) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_hMain;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_hIcon;
    wcscpy_s(nid.szTip, BuildTrayTip().c_str());
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void RemoveTrayIcon()
{
    if (!g_trayAdded || !g_hMain) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_hMain;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void OnTrayCommand(HWND hwnd, LPARAM lParam)
{
    switch (lParam)
    {
    case WM_LBUTTONUP:
        if (IsSettingsWindowVisible())
            HideSettingsWindow(); // 托盘左键关闭：矩形落盘+隐藏（与 WM_CLOSE 同路径）
        else
            CreateSettingsWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        return;

    case WM_MBUTTONUP: // 中键托盘 = 使用帮助（左键已被设置占用，帮助常驻无冲突）
        ShowHelpWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        return;

    case WM_RBUTTONUP:
    {
        SetForegroundWindow(hwnd); // 菜单在外部点击时正确消失
        HMENU menu = CreatePopupMenu();
        Settings s = LoadSettings();
        AppendMenuW(menu, MF_STRING | (s.pinyinEnabled ? MF_CHECKED : 0), 2001, L"拼音组合模式");
        AppendMenuW(menu, MF_STRING | (s.paused ? MF_CHECKED : 0), 2004, L"暂停预测（完全直通）");
        AppendMenuW(menu, MF_STRING, 2012, L"暂停 30 分钟后自动恢复");
        AppendMenuW(menu, MF_STRING, 2016, L"暂停 1 小时后自动恢复");
    AppendMenuW(menu, MF_STRING, 2017, L"打字统计...");
        AppendMenuW(menu, MF_STRING | (s.candidatePinned ? MF_CHECKED : 0), 2007, L"固定候选窗位置");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, 2005, L"符号面板（Ctrl+Shift+B）");
        AppendMenuW(menu, MF_STRING, 2002, L"设置...");
        AppendMenuW(menu, MF_STRING, 2009, L"使用帮助...");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, 2010, L"导出设置备份...");
        AppendMenuW(menu, MF_STRING, 2011, L"导入设置备份...");
        AppendMenuW(menu, MF_STRING, 2013, L"重新加载用户词典");
        AppendMenuW(menu, MF_STRING, 2014, L"打开数据文件夹");
        AppendMenuW(menu, MF_STRING, 2015, L"恢复默认设置...");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                    2008, L"开机自动启动");
        AppendMenuW(menu, MF_STRING, 2006, L"卸载灵犀输入法...");
        AppendMenuW(menu, MF_STRING, 2003, L"退出");
        POINT pt;
        GetCursorPos(&pt);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        switch (cmd)
        {
        case 2001:
        {
            Settings ns = LoadSettings();
            ns.pinyinEnabled = !ns.pinyinEnabled;
            SaveSettings(ns);
            RefreshSettingsControls(); // 窗口开着则同步控件
            break;
        }
        case 2004:
        {
            Settings ns = LoadSettings();
            ns.paused = !ns.paused;
            SaveSettings(ns);
            KillTimer(hwnd, 2); // 手动切换暂停态则取消"30 分钟自动恢复"定时
            UpdateTrayTip(); // 暂停态变化 → 托盘悬停文字同步
            break;
        }
        case 2012: // 暂停 30 分钟：定时到自动恢复（写作/演示等场景的临时静音）
        {
            Settings ns = LoadSettings();
            ns.paused = 1;
            SaveSettings(ns);
            SetTimer(hwnd, 2, 30 * 60 * 1000, NULL);
            UpdateTrayTip();
            LX_LOG(L"ui", L"暂停 30 分钟（定时自动恢复）");
            break;
        }
        case 2016: // 暂停 1 小时：同"30 分钟"定时器，到点自动恢复
        {
            Settings ns = LoadSettings();
            ns.paused = 1;
            SaveSettings(ns);
            SetTimer(hwnd, 2, 60 * 60 * 1000, NULL);
            UpdateTrayTip();
            LX_LOG(L"ui", L"暂停 1 小时（定时自动恢复）");
            break;
        }
        case 2013:
            if (g_reloadUserDict) g_reloadUserDict(); // 词典文件外部编辑后免重启生效
            break;
        case 2014:
        {
            // 打开数据文件夹（用户词典/设置备份所在地；取词典路径的父目录，
            // 与实际数据位置永远一致）
            std::wstring dictPath = UserDictPathW();
            size_t slash = dictPath.rfind(L'\\');
            std::wstring dir = (slash != std::wstring::npos)
                               ? dictPath.substr(0, slash) : dictPath;
            CreateDirectoryW(dir.c_str(), NULL); // 首次可能不存在
            ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case 2015:
        {
            if (MessageBoxW(hwnd,
                    L"将清除全部设置恢复默认（用户词典与热键短语保留），确定？",
                    L"恢复默认设置", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            {
                ResetSettingsToDefaults();
                if (g_settingsChanged) g_settingsChanged(); // 热键按默认重注册
                UpdateTrayTip();
                RefreshSettingsControls(); // 设置窗开着则同步控件
                LX_LOG(L"ui", L"已恢复默认设置（注册表值已清除）");
            }
            break;
        }
        case 2008:
            SetAutoStartEnabled(!IsAutoStartEnabled()); // 勾选态以 HKCU\Run 为准
            break;
        case 2007:
            // 勾选态由 main 侧 CandWindow::TogglePin 按窗口实际位置写入
            if (g_reqPinToggle) g_reqPinToggle();
            break;
        case 2005:
            if (g_showSymbols) g_showSymbols();
            break;
        case 2006:
        {
            // 一键卸载：确认后提权跑 tools\uninstall.cmd（注销输入法+清注册表/
            // 用户数据+零残留校验）。本进程随卸载脚本 taskkill 退出。
            if (MessageBoxW(hwnd,
                    L"将彻底卸载灵犀输入法：\n"
                    L"  · 注销输入法（COM/CTF 注册表，需 UAC 管理员确认）\n"
                    L"  · 从语言列表移除灵犀、清理全部注册表痕迹\n"
                    L"  · 删除用户数据（已学习词典、设置）\n"
                    L"  · 卸载后自动校验零残留\n\n"
                    L"确定要继续吗？",
                    L"卸载灵犀输入法", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            {
                wchar_t dir[MAX_PATH] = {};
                GetModuleFileNameW(NULL, dir, MAX_PATH);
                wchar_t* slash = wcsrchr(dir, L'\\');
                if (slash)
                {
                    slash[1] = 0; // 截到 bin/Debug 目录
                    wchar_t script[MAX_PATH * 2];
                    _snwprintf_s(script, _TRUNCATE, L"%s..\\..\\tools\\uninstall.cmd", dir);
                    wchar_t params[MAX_PATH * 2 + 16];
                    _snwprintf_s(params, _TRUNCATE, L"/c \"%s\"", script);
                    ShellExecuteW(NULL, L"runas", L"cmd.exe", params, NULL, SW_SHOWNORMAL);
                }
            }
            break;
        }
        case 2002:
            CreateSettingsWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            break;
        case 2009:
            ShowHelpWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            break;
        case 2017:
            ShowUsageStatsWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            break;
        case 2010:
            ExportSettingsBackup(hwnd);
            break;
        case 2011:
            ImportSettingsBackup(hwnd);
            break;
        case 2003:
            PostQuitMessage(0);
            break;
        }
        return;
    }
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg)
    {
        AddTrayIcon(); // explorer 重启后恢复托盘
        return 0;
    }
    switch (msg)
    {
    case WM_APP_TRAY:
        OnTrayCommand(hwnd, lParam);
        return 0;
    case WM_HOTKEY:
        // Ctrl+Alt+P：切换暂停预测（写作/游戏临时静音的全局快捷键；
        // 与托盘菜单切换同语义——清"30 分钟"定时 + tip 同步）
        if (wParam == (WPARAM)kTrayHotkeyPause)
        {
            Settings ns = LoadSettings();
            ns.paused = !ns.paused;
            SaveSettings(ns);
            KillTimer(hwnd, 2);
            UpdateTrayTip();
            LX_LOG(L"ui", L"Ctrl+Alt+P 切换暂停 paused=%u", (unsigned)ns.paused);
        }
        // Ctrl+Alt+H：全局热键打开使用帮助（速查页随时可查）
        else if (wParam == (WPARAM)kTrayHotkeyHelp)
        {
            ShowHelpWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            LX_LOG(L"ui", L"Ctrl+Alt+H 打开使用帮助");
        }
        return 0;
    case WM_TIMER:
        // "暂停 30 分钟"到期：自动恢复预测（用户中途手动切换过则定时已被清）
        if (wParam == 2)
        {
            KillTimer(hwnd, 2);
            Settings ns = LoadSettings();
            ns.paused = 0;
            SaveSettings(ns);
            UpdateTrayTip();
            LX_LOG(L"ui", L"30 分钟暂停到期，已自动恢复预测");
        }
        return 0;
    case WM_ENDSESSION:
        StatsFlush(); // 注销/关机：未落盘的打字统计（≤20 次）补写
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, kTrayHotkeyPause);
        UnregisterHotKey(hwnd, kTrayHotkeyHelp);
        RemoveTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ---------------- 跨文件共享（uiinternal.h）----------------

HICON g_hIcon = nullptr;

void UpdateTrayTip()
{
    if (!g_trayAdded || !g_hMain) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_hMain;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcscpy_s(nid.szTip, BuildTrayTip().c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool IsAutoStartEnabled()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    bool found = RegQueryValueExW(hk, L"LingxiIme", NULL, NULL, NULL, NULL)
                 == ERROR_SUCCESS;
    RegCloseKey(hk);
    return found;
}

void InitTrayAndSettings(HINSTANCE hInst)
{
    g_hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1)); // app.rc: 1 ICON "app.ico"
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.lpszClassName = kMainWndClass;
    RegisterClassExW(&wc);

    g_hMain = CreateWindowExW(0, kMainWndClass, L"", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    AddTrayIcon();
    // 全局热键 Ctrl+Alt+P 切换暂停预测（被占用仅记日志，不阻断启动）
    if (!RegisterHotKey(g_hMain, kTrayHotkeyPause,
                        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, (UINT)'P'))
        LX_LOG(L"ui", L"Ctrl+Alt+P 热键注册失败（组合键被其他程序占用）");
    // 全局热键 Ctrl+Alt+H 打开使用帮助（同款容错）
    if (!RegisterHotKey(g_hMain, kTrayHotkeyHelp,
                        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, (UINT)'H'))
        LX_LOG(L"ui", L"Ctrl+Alt+H 热键注册失败（组合键被其他程序占用）");
}

} // namespace lingxi
