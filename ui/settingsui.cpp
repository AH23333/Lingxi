//
// 托盘图标 + 设置窗口。
//
// 设置存 HKCU\Software\LingxiIme（common/settings），DLL 与 UI 每次读取实时生效：
// - 拼音组合模式：DLL 在模式检测（焦点/激活/语言变化）时读取
// - 候选数量 / 界面明暗 / 英文逐键补全：UI 在每次预测请求时读取
//
#include "settingsui.h"

#include <shellapi.h>
#include <commdlg.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <functional>

#include "../common/log.h"
#include "../common/settings.h"

namespace lingxi {

namespace {

constexpr wchar_t kMainWndClass[] = L"LingxiUIMainWnd";
constexpr wchar_t kSettingsWndClass[] = L"LingxiSettingsWnd";
constexpr wchar_t kUserDictWndClass[] = L"LingxiUserDictWnd";
constexpr UINT WM_APP_TRAY = WM_APP + 10; // 托盘回调消息

std::function<void()> g_reloadUserDict; // 用户词典变更后重新加载引擎
std::function<void()> g_showSymbols;   // 托盘菜单：打开符号面板
std::function<void()> g_settingsChanged; // 设置保存后（同步热键等）

// ---------------- 用户词典文件工具（UTF-8：词\t拼音\t次数）----------------

std::wstring UserDictPathW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring dir = std::wstring(buf) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\userdict.txt";
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe) + L"userdict.txt";
}

std::string WToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, NULL, NULL);
    return s;
}

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

bool ReadFileBytes(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    out.resize(size);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), size, &got, NULL);
    CloseHandle(h);
    out.resize(ok ? got : 0);
    return ok != FALSE;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data)
{
    // 原子写：先写 .tmp 再 MoveFileEx 替换。引擎（管道线程自学习落盘）与本
    // 窗口（UI 线程清空/导入）是同一文件的两个写者，直写 CREATE_ALWAYS 撞车
    // 会产生截断/半截文件；替换式写入保证读者永远看到完整内容。
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, NULL);
    CloseHandle(h);
    if (!ok || put != (DWORD)data.size())
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

struct UdEntry { std::wstring word; std::wstring key; int count; };

// 解析用户词典文本为条目（兼容 UTF-8 BOM）。
std::vector<UdEntry> ParseUserDict(const std::string& raw)
{
    std::string text = raw;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    std::vector<UdEntry> v;
    size_t start = 0;
    while (start < text.size())
    {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        UdEntry e;
        e.word = Utf8ToW(line.substr(0, t1));
        e.key = Utf8ToW(line.substr(t1 + 1, t2 - t1 - 1));
        e.count = atoi(line.substr(t2 + 1).c_str());
        if (!e.word.empty() && !e.key.empty() && e.count > 0) v.push_back(std::move(e));
    }
    return v;
}

std::string SerializeUserDict(const std::vector<UdEntry>& entries)
{
    std::string out;
    for (const auto& e : entries)
    {
        out += WToUtf8(e.word);
        out.push_back('\t');
        out += WToUtf8(e.key);
        out.push_back('\t');
        out += std::to_string(e.count);
        out.push_back('\n');
    }
    return out;
}


constexpr int IDC_CHK_PINYIN = 101;
constexpr int IDC_CHK_EN = 102;
constexpr int IDC_EDT_COUNT = 104;
constexpr int IDC_CMB_FONTSIZE = 105;
constexpr int IDC_CMB_DARK = 106;
constexpr int IDC_CHK_CNPUNCT = 107;
constexpr int IDC_CHK_FUZZY = 108;
constexpr int IDC_CHK_SHIFTTOGGLE = 109;
constexpr int IDC_CHK_VMODE = 110;
constexpr int IDC_BTN_USERDICT = 111;
constexpr int IDC_CHK_SHUANGPIN = 112;
constexpr int IDC_CHK_SYMBOLHOTKEY = 113;
constexpr int IDC_CHK_TRADITIONAL = 114;
constexpr int IDC_CMB_SKIN = 115;
constexpr int IDC_CHK_LLMRERANK = 116;

HWND g_hMain = nullptr;    // 隐藏消息窗口（托盘宿主）
HWND g_hSettings = nullptr; // 设置窗口（关闭=隐藏，常驻）
HWND g_hChkPinyin = nullptr, g_hChkEn = nullptr, g_hEdtCount = nullptr,
     g_hCmbDark = nullptr, g_hCmbFontSize = nullptr, g_hCmbSkin = nullptr,
     g_hChkCnPunct = nullptr, g_hChkFuzzy = nullptr,
     g_hChkShiftToggle = nullptr, g_hChkVMode = nullptr,
     g_hChkShuangpin = nullptr, g_hChkSymbolHotkey = nullptr, g_hChkTraditional = nullptr,
     g_hChkLlmRerank = nullptr;
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
    Button_SetCheck(g_hChkCnPunct, s.cnPunct ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkFuzzy, s.fuzzyPinyin ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkShiftToggle, s.shiftToggle ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkVMode, s.vMode ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkShuangpin, s.shuangpin ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkSymbolHotkey, s.symbolHotkey ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkTraditional, s.traditionalOutput ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_hChkLlmRerank, s.llmRerank ? BST_CHECKED : BST_UNCHECKED);
    wchar_t buf[16];
    _snwprintf_s(buf, _TRUNCATE, L"%u", s.candidateCount);
    Edit_SetText(g_hEdtCount, buf);
    static const uint32_t kFontSizes[] = {12, 14, 16, 18, 20, 24, 28, 32};
    int fsSel = 2;
    for (int i = 0; i < 8; i++)
        if (kFontSizes[i] == s.candidateFontSize) fsSel = i;
    ComboBox_SetCurSel(g_hCmbFontSize, fsSel);
    ComboBox_SetCurSel(g_hCmbSkin, (int)s.skinIndex);
    ComboBox_SetCurSel(g_hCmbDark, (int)s.darkMode);
    g_updating = false;
}

void SaveFromControls()
{
    if (g_updating) return;
    Settings s = LoadSettings(); // 保留未在此窗口编辑的字段
    s.pinyinEnabled = Button_GetCheck(g_hChkPinyin) == BST_CHECKED;
    s.enCompletion = Button_GetCheck(g_hChkEn) == BST_CHECKED;
    s.cnPunct = Button_GetCheck(g_hChkCnPunct) == BST_CHECKED;
    s.fuzzyPinyin = Button_GetCheck(g_hChkFuzzy) == BST_CHECKED;
    s.shiftToggle = Button_GetCheck(g_hChkShiftToggle) == BST_CHECKED;
    s.vMode = Button_GetCheck(g_hChkVMode) == BST_CHECKED;
    s.shuangpin = Button_GetCheck(g_hChkShuangpin) == BST_CHECKED;
    s.symbolHotkey = Button_GetCheck(g_hChkSymbolHotkey) == BST_CHECKED;
    s.traditionalOutput = Button_GetCheck(g_hChkTraditional) == BST_CHECKED;
    s.llmRerank = Button_GetCheck(g_hChkLlmRerank) == BST_CHECKED;
    wchar_t buf[16] = L"";
    Edit_GetText(g_hEdtCount, buf, 16);
    int n = _wtoi(buf);
    if (n < 1) n = 1;
    if (n > 9) n = 9;
    s.candidateCount = (uint32_t)n;
    static const uint32_t kFontSizesSave[] = {12, 14, 16, 18, 20, 24, 28, 32};
    int fsIdx = ComboBox_GetCurSel(g_hCmbFontSize);
    if (fsIdx >= 0 && fsIdx < 8) s.candidateFontSize = kFontSizesSave[fsIdx];
    int skinSel = ComboBox_GetCurSel(g_hCmbSkin);
    if (skinSel >= 0 && skinSel <= 4) s.skinIndex = (uint32_t)skinSel;
    int sel = ComboBox_GetCurSel(g_hCmbDark);
    if (sel >= 0) s.darkMode = (uint32_t)sel;
    SaveSettings(s);
    if (g_settingsChanged) g_settingsChanged();
    LX_LOG(L"ui", L"设置已保存: pinyin=%d count=%u dark=%u en=%d cnPunct=%d fuzzy=%d shift=%d v=%d",
           (int)s.pinyinEnabled, s.candidateCount, s.darkMode, (int)s.enCompletion,
           (int)s.cnPunct, (int)s.fuzzyPinyin, (int)s.shiftToggle, (int)s.vMode);
}

// ---------------- 用户词典管理窗口 ----------------

constexpr int IDC_UD_LIST = 300;
constexpr int IDC_UD_STATIC = 301;
constexpr int IDC_UD_DEL = 302;
constexpr int IDC_UD_CLEAR = 303;
constexpr int IDC_UD_IMPORT = 304;
constexpr int IDC_UD_EXPORT = 305;
constexpr int IDC_UD_CLOSE = 306;

HWND g_hUserDict = nullptr;
HWND g_hUdList = nullptr;

void RefreshUdList()
{
    if (!g_hUdList) return;
    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    SendMessageW(g_hUdList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < entries.size(); i++)
    {
        wchar_t line[256];
        _snwprintf_s(line, _TRUNCATE, L"%ls    %ls    %d",
                     entries[i].word.c_str(), entries[i].key.c_str(), entries[i].count);
        SendMessageW(g_hUdList, LB_ADDSTRING, 0, (LPARAM)line);
    }
    wchar_t cap[64];
    _snwprintf_s(cap, _TRUNCATE, L"已学习词条：%zu", entries.size());
    SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, cap);
}

// 写回条目并让引擎立即重新加载。
void PersistUdEntries(const std::vector<UdEntry>& entries)
{
    if (!WriteFileBytes(UserDictPathW(), SerializeUserDict(entries)))
    {
        // 静默丢数据比失败更糟：明示用户（文件被占用/无权限）。
        MessageBoxW(g_hUserDict, L"用户词典写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_reloadUserDict) g_reloadUserDict();
}

void UdDeleteSelected()
{
    int sel = (int)SendMessageW(g_hUdList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    if (sel < (int)entries.size()) entries.erase(entries.begin() + sel);
    PersistUdEntries(entries);
    RefreshUdList();
}

void UdClearAll()
{
    if (MessageBoxW(g_hUserDict, L"确定清空全部已学习词条？此操作不可撤销。",
                    L"清空用户词典", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    WriteFileBytes(UserDictPathW(), std::string());
    if (g_reloadUserDict) g_reloadUserDict();
    RefreshUdList();
}

void UdImport()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hUserDict;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string inRaw, curRaw;
    if (!ReadFileBytes(path, inRaw)) return;
    std::vector<UdEntry> imported = ParseUserDict(inRaw);
    std::vector<UdEntry> cur;
    if (ReadFileBytes(UserDictPathW(), curRaw)) cur = ParseUserDict(curRaw);
    // 合并：同 (word,key) 累加 count，否则追加
    for (const auto& ie : imported)
    {
        bool merged = false;
        for (auto& ce : cur)
            if (ce.word == ie.word && ce.key == ie.key) { ce.count += ie.count; merged = true; break; }
        if (!merged) cur.push_back(ie);
    }
    PersistUdEntries(cur);
    RefreshUdList();
}

void UdExport()
{
    wchar_t path[MAX_PATH] = L"userdict_export.txt";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hUserDict;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::string raw;
    if (ReadFileBytes(UserDictPathW(), raw)) WriteFileBytes(path, raw);
}

LRESULT CALLBACK UserDictWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_UD_DEL: UdDeleteSelected(); return 0;
        case IDC_UD_CLEAR: UdClearAll(); return 0;
        case IDC_UD_IMPORT: UdImport(); return 0;
        case IDC_UD_EXPORT: UdExport(); return 0;
        case IDC_UD_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateUserDictWindow(HINSTANCE hInst)
{
    if (g_hUserDict) { ShowWindow(g_hUserDict, SW_SHOW); SetForegroundWindow(g_hUserDict); RefreshUdList(); return; }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = UserDictWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kUserDictWndClass;
    RegisterClassExW(&wc);

    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int W = MulDiv(420, dpi, 96), H = MulDiv(400, dpi, 96);
    int btnH = MulDiv(28, dpi, 96);
    int pad = MulDiv(12, dpi, 96);

    g_hUserDict = CreateWindowExW(0, kUserDictWndClass, L"用户词典管理",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, hInst, NULL);

    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                    pad, pad, W - pad * 2, btnH, g_hUserDict, (HMENU)(INT_PTR)IDC_UD_STATIC, hInst, NULL);
    int listTop = pad + btnH + 4;
    int btnRowY = H - btnH - pad - 8;
    g_hUdList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                pad, listTop, W - pad * 2, btnRowY - listTop - 8,
                                g_hUserDict, (HMENU)(INT_PTR)IDC_UD_LIST, hInst, NULL);

    struct Btn { const wchar_t* text; int id; };
    Btn btns[] = {
        { L"删除选中", IDC_UD_DEL }, { L"清空", IDC_UD_CLEAR },
        { L"导入", IDC_UD_IMPORT }, { L"导出", IDC_UD_EXPORT },
        { L"关闭", IDC_UD_CLOSE },
    };
    int bw = (W - pad * 2 - 8 * 4) / 5;
    for (int i = 0; i < 5; i++)
    {
        int bx = pad + i * (bw + 8);
        CreateWindowExW(0, L"BUTTON", btns[i].text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        bx, btnRowY, bw, btnH, g_hUserDict,
                        (HMENU)(INT_PTR)btns[i].id, hInst, NULL);
    }

    EnumChildWindows(g_hUserDict, [](HWND h, LPARAM lp) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)g_hFont);

    RefreshUdList();
    ShowWindow(g_hUserDict, SW_SHOW);
    SetForegroundWindow(g_hUserDict);
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
        case IDC_CHK_CNPUNCT:
        case IDC_CHK_FUZZY:
        case IDC_CHK_SHIFTTOGGLE:
        case IDC_CHK_VMODE:
        case IDC_CHK_SHUANGPIN:
        case IDC_CHK_SYMBOLHOTKEY:
        case IDC_CHK_TRADITIONAL:
        case IDC_CHK_LLMRERANK:
            if (HIWORD(wParam) == BN_CLICKED) SaveFromControls();
            return 0;
        case IDC_EDT_COUNT:
            if (HIWORD(wParam) == EN_CHANGE) SaveFromControls();
            return 0;
        case IDC_CMB_DARK:
        case IDC_CMB_FONTSIZE:
        case IDC_CMB_SKIN:
            if (HIWORD(wParam) == CBN_SELCHANGE) SaveFromControls();
            return 0;
        case IDC_BTN_USERDICT:
            CreateUserDictWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
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
    int W = MulDiv(440, scale, 96), H = MulDiv(704, scale, 96);
    int x = MulDiv(16, scale, 96), y = MulDiv(16, scale, 96);
    int rowH = MulDiv(30, scale, 96);

    // 关键：创建子控件期间 Edit 会触发 EN_CHANGE、复选框初始为未勾选，若此时
    // 回写会把所有设置污染为 0。整个控件创建期间屏蔽保存，直到 LoadToControls
    // 正确加载后才放开（LoadToControls 结束时会把 g_updating 复位为 false）。
    g_updating = true;

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
    g_hChkFuzzy = mkCheck(L"模糊音（z/zh、c/ch、s/sh、n/l、前后鼻音互通，精确优先）", IDC_CHK_FUZZY);
    g_hChkCnPunct = mkCheck(L"中文标点（. , / ; 等自动转全角，Shift 出 ？：》《 等）", IDC_CHK_CNPUNCT);
    g_hChkShiftToggle = mkCheck(L"Shift 单击切换中/英文（无拼音时单击 Shift，显示中/英提示）", IDC_CHK_SHIFTTOGGLE);
    g_hChkVMode = mkCheck(L"v 模式（中文下首字母 v 直通英文，如 vhello 出 hello）", IDC_CHK_VMODE);
    g_hChkShuangpin = mkCheck(L"小鹤双拼（每音节两键：声母+韵母，零声母按韵母首字母补齐）", IDC_CHK_SHUANGPIN);
    g_hChkSymbolHotkey = mkCheck(L"符号面板热键 Ctrl+Shift+B（全局独占，会占用应用同款快捷键如 Edge 插件；默认关，面板可从托盘打开）", IDC_CHK_SYMBOLHOTKEY);
    g_hChkTraditional = mkCheck(L"繁体输出（候选简体字转为繁体）", IDC_CHK_TRADITIONAL);
    g_hChkLlmRerank = mkCheck(L"LLM 上下文智能重排（实验：停顿打完一字后按上文重排候选，增加 CPU）", IDC_CHK_LLMRERANK);

    y += MulDiv(6, scale, 96);
    mkStatic(L"英文补全候选数（1-9，仅英文逐键补全时生效）", 0);
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
    y += rowH + MulDiv(6, scale, 96);

    // 候选字号（12-32，即时生效）
    mkStatic(L"候选字号", 0);
    g_hCmbFontSize = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                     x, y, MulDiv(100, scale, 96), rowH * 6,
                                     g_hSettings, (HMENU)(INT_PTR)IDC_CMB_FONTSIZE, hInst, NULL);
    for (uint32_t z : {12u, 14u, 16u, 18u, 20u, 24u, 28u, 32u})
    {
        wchar_t b[8];
        _snwprintf_s(b, _TRUNCATE, L"%u", z);
        ComboBox_AddString(g_hCmbFontSize, b);
    }
    y += rowH + MulDiv(6, scale, 96);

    // 界面配色（候选窗强调色，即时生效）
    mkStatic(L"界面配色", 0);
    g_hCmbSkin = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                 x, y, MulDiv(100, scale, 96), rowH * 6,
                                 g_hSettings, (HMENU)(INT_PTR)IDC_CMB_SKIN, hInst, NULL);
    for (const wchar_t* s : {L"蓝色", L"青色", L"紫色", L"绿色", L"橙色"})
        ComboBox_AddString(g_hCmbSkin, s);
    y += rowH + MulDiv(6, scale, 96);

    // 用户词典管理：查看/删除/清空/导入导出已学习词条
    CreateWindowExW(0, L"BUTTON", L"用户词典管理...",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    x, y, MulDiv(180, scale, 96), rowH, g_hSettings,
                    (HMENU)(INT_PTR)IDC_BTN_USERDICT, hInst, NULL);

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
        AppendMenuW(menu, MF_STRING, 2005, L"符号面板（Ctrl+Shift+B）");
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
        case 2005:
            if (g_showSymbols) g_showSymbols();
            break;
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

void SetUserDictReloadCallback(std::function<void()> cb)
{
    g_reloadUserDict = std::move(cb);
}

void SetShowSymbolsCallback(std::function<void()> cb)
{
    g_showSymbols = std::move(cb);
}

void SetSettingsChangedCallback(std::function<void()> cb)
{
    g_settingsChanged = std::move(cb);
}

void ShowSettingsWindow(HINSTANCE hInst)
{
    CreateSettingsWindow(hInst);
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
}

} // namespace lingxi
