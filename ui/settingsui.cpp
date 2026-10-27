//
// 托盘图标 + 设置窗口。
//
// 设置存 HKCU\Software\LingxiIme（common/settings），DLL 与 UI 每次读取实时生效：
// - 拼音组合模式：DLL 在模式检测（焦点/激活/语言变化）时读取
// - 候选数量 / 界面明暗 / 英文逐键补全：UI 在每次预测请求时读取
//
#include "settingsui.h"

#include <shellapi.h>
#include <windowsx.h>

#include "../common/log.h"
#include "../common/settings.h"

namespace lingxi {

namespace {

constexpr wchar_t kMainWndClass[] = L"LingxiUIMainWnd";
constexpr wchar_t kSettingsWndClass[] = L"LingxiSettingsWnd";
constexpr UINT WM_APP_TRAY = WM_APP + 10; // 托盘回调消息

constexpr int IDC_CHK_PINYIN = 101;
constexpr int IDC_CHK_EN = 102;
constexpr int IDC_EDT_COUNT = 104;
constexpr int IDC_CMB_DARK = 106;

HWND g_hMain = nullptr;    // 隐藏消息窗口（托盘宿主）
HWND g_hSettings = nullptr; // 设置窗口（关闭=隐藏，常驻）
HWND g_hChkPinyin = nullptr, g_hChkEn = nullptr, g_hEdtCount = nullptr, g_hCmbDark = nullptr;
HFONT g_hFont = nullptr;
bool g_updating = false; // 程序性改控件时不回写设置

HICON g_hIcon = nullptr;
UINT g_taskbarCreatedMsg = 0;
bool g_trayAdded = false;

// ---------------- 设置读写 ----------------

void LoadToControls()
{
    Settings s = LoadSettings();
    g_updating = true;
    Button_SetCheck(g_hChkPinyin, s.pinyinEnabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkEn, s.enCompletion ? BST_CHECKED : BST_UNCHECKED);
    wchar_t buf[16];
    _snwprintf_s(buf, _TRUNCATE, L"%u", s.candidateCount);
    Edit_SetText(g_hEdtCount, buf);
    ComboBox_SetCurSel(g_hCmbDark, (int)s.darkMode);
    g_updating = false;
}

void SaveFromControls()
{
    if (g_updating) return;
    Settings s = LoadSettings(); // 保留未在此窗口编辑的字段
    s.pinyinEnabled = Button_GetCheck(g_hChkPinyin) == BST_CHECKED;
    s.enCompletion = Button_GetCheck(g_hChkEn) == BST_CHECKED;
    wchar_t buf[16] = L"";
    Edit_GetText(g_hEdtCount, buf, 16);
    int n = _wtoi(buf);
    if (n < 1) n = 1;
    if (n > 9) n = 9;
    s.candidateCount = (uint32_t)n;
    int sel = ComboBox_GetCurSel(g_hCmbDark);
    if (sel >= 0) s.darkMode = (uint32_t)sel;
    SaveSettings(s);
    LX_LOG(L"ui", L"设置已保存: pinyin=%d count=%u dark=%u en=%d",
           (int)s.pinyinEnabled, s.candidateCount, s.darkMode, (int)s.enCompletion);
}

// ---------------- 设置窗口 ----------------

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CHK_PINYIN:
        case IDC_CHK_EN:
            if (HIWORD(wParam) == BN_CLICKED) SaveFromControls();
            return 0;
        case IDC_EDT_COUNT:
            if (HIWORD(wParam) == EN_CHANGE) SaveFromControls();
            return 0;
        case IDC_CMB_DARK:
            if (HIWORD(wParam) == CBN_SELCHANGE) SaveFromControls();
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); // 关闭=隐藏，保留控件状态
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateSettingsWindow(HINSTANCE hInst)
{
    if (g_hSettings) { ShowWindow(g_hSettings, SW_SHOW); SetForegroundWindow(g_hSettings); return; }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);

    // 客户区 380x300，随 DPI 简单缩放（96 基准）
    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int scale = (int)dpi;
    int W = MulDiv(400, scale, 96), H = MulDiv(300, scale, 96);
    int x = MulDiv(16, scale, 96), y = MulDiv(16, scale, 96);
    int rowH = MulDiv(30, scale, 96);

    g_hSettings = CreateWindowExW(0, kSettingsWndClass, L"灵犀输入法设置",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, hInst, NULL);

    g_hFont = CreateFontW(-MulDiv(12, scale, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    auto mkStatic = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                 x, y, W - x * 2, rowH, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        y += rowH;
        return h;
    };
    auto mkCheck = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 x, y, W - x * 2, rowH, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        y += rowH + MulDiv(4, scale, 96);
        return h;
    };

    mkStatic(L"拼音（zh-CN 配置下生效）", 0);
    g_hChkPinyin = mkCheck(L"拼音组合模式（字母上屏汉字，空格/数字选择；关闭则字母直通+英文补全）", IDC_CHK_PINYIN);
    g_hChkEn = mkCheck(L"英文逐键补全（词中自动补全英文单词）", IDC_CHK_EN);

    y += MulDiv(6, scale, 96);
    mkStatic(L"候选数量（1-9，数字键选择）", 0);
    g_hEdtCount = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"9",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                  x, y, MulDiv(60, scale, 96), rowH, g_hSettings, (HMENU)(INT_PTR)IDC_EDT_COUNT, hInst, NULL);
    y += rowH + MulDiv(8, scale, 96);

    mkStatic(L"候选窗明暗", 0);
    g_hCmbDark = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                 x, y, MulDiv(160, scale, 96), rowH * 4, g_hSettings, (HMENU)(INT_PTR)IDC_CMB_DARK, hInst, NULL);
    ComboBox_AddString(g_hCmbDark, L"跟随应用");
    ComboBox_AddString(g_hCmbDark, L"浅色");
    ComboBox_AddString(g_hCmbDark, L"深色");

    y += rowH + MulDiv(10, scale, 96);
    mkStatic(L"修改即时生效并自动保存。重建输入法后需重启应用才会加载新版本。", 0);

    EnumChildWindows(g_hSettings, [](HWND h, LPARAM lp) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)g_hFont);

    LoadToControls();
    ShowWindow(g_hSettings, SW_SHOW);
    SetForegroundWindow(g_hSettings);
}

// ---------------- 托盘 ----------------

void AddTrayIcon()
{
    if (!g_hMain || !g_hIcon) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_hMain;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_hIcon;
    wcscpy_s(nid.szTip, L"灵犀输入法");
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
        if (g_hSettings && IsWindowVisible(g_hSettings))
            ShowWindow(g_hSettings, SW_HIDE);
        else
            CreateSettingsWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        return;

    case WM_RBUTTONUP:
    {
        SetForegroundWindow(hwnd); // 菜单在外部点击时正确消失
        HMENU menu = CreatePopupMenu();
        Settings s = LoadSettings();
        AppendMenuW(menu, MF_STRING | (s.pinyinEnabled ? MF_CHECKED : 0), 2001, L"拼音组合模式");
        AppendMenuW(menu, MF_STRING | (s.paused ? MF_CHECKED : 0), 2004, L"暂停预测（完全直通）");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, 2002, L"设置...");
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
            if (g_hChkPinyin) LoadToControls(); // 窗口开着则同步控件
            break;
        }
        case 2004:
        {
            Settings ns = LoadSettings();
            ns.paused = !ns.paused;
            SaveSettings(ns);
            break;
        }
        case 2002:
            CreateSettingsWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
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
    case WM_DESTROY:
        RemoveTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

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
}

} // namespace lingxi
