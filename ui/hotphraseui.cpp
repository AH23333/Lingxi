//
// 快捷短语：存储 + 管理窗口（热键录制编辑框 + 列表 + 添加/删除）。
// 热键注册在 main.cpp（RegisterHotKey id 100 起）；保存后经
// SetHotPhrasesChangedCallback 通知重新注册。DLL 侧零改动——插入走
// 既有的 PipeServer::SendInsertText → MSG_INSERT_TEXT 链路。
//
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hotphraseui.h"
#include "../common/log.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kHpWndClass[] = L"LingxiHotPhraseWnd";

constexpr int IDC_HP_LIST = 400;
constexpr int IDC_HP_HOTEDIT = 401; // 热键录制框（子类化 EDIT）
constexpr int IDC_HP_PHRASE = 402;  // 句式内容
constexpr int IDC_HP_ADD = 403;
constexpr int IDC_HP_DEL = 404;
constexpr int IDC_HP_CLOSE = 405;
constexpr int IDC_HP_STATUS = 406;

std::function<void()> g_hpChanged;

// ---------------- 存储层 ----------------

std::wstring HotPhrasePathW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring dir = std::wstring(buf) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\hotphrases.txt";
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe) + L"hotphrases.txt";
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
    // 原子写：.tmp 替换式（同用户词典——避免读写者撞车产生半截文件）
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, NULL);
    CloseHandle(h);
    if (!ok || wrote != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// ---------------- 管理窗口 ----------------

HWND g_hHpWnd = nullptr;
HWND g_hHpList = nullptr;
WNDPROC g_oldHotEditProc = nullptr;
uint32_t g_capMods = 0; // 录制框当前捕获（0 = 未捕获）
uint32_t g_capVk = 0;

// WinUI 风格：微软雅黑 UI 12pt（进程内复用）
HFONT HpFont()
{
    static HFONT f = nullptr;
    if (!f)
    {
        UINT dpi = 96;
        if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                           "GetDpiForSystem"))
            dpi = pGetDpi();
        f = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }
    return f;
}

// 窗体底中性灰（与设置页一致）
HBRUSH HpBgBrush()
{
    static HBRUSH b = CreateSolidBrush(kUiBg);
    return b;
}

std::wstring VkName(uint32_t vk)
{
    if (vk >= L'A' && vk <= L'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= L'0' && vk <= L'9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F12)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    wchar_t name[32] = {};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), name, 32) && name[0])
        return name;
    wchar_t hex[16] = {};
    _snwprintf_s(hex, _TRUNCATE, L"键(0x%02X)", vk);
    return hex;
}

std::vector<HotPhrase> LoadEntries()
{
    std::string raw;
    std::vector<HotPhrase> out;
    if (!ReadFileBytes(HotPhrasePathW(), raw)) return out;
    std::wstring text = Utf8ToW(raw);
    size_t pos = 0;
    while (out.size() < kMaxHotPhrases)
    {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = text.substr(pos,
            (eol == std::wstring::npos ? text.size() : eol) - pos);
        pos = (eol == std::wstring::npos) ? text.size() : eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line[0] == L'\xFEFF') line.erase(0, 1); // BOM
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        size_t comma = line.find(L',');
        if (comma == std::wstring::npos || comma > tab) continue;
        HotPhrase hp;
        hp.mods = (uint32_t)wcstoul(line.substr(0, comma).c_str(), NULL, 10);
        hp.vk = (uint32_t)wcstoul(line.substr(comma + 1, tab - comma - 1).c_str(), NULL, 10);
        hp.phrase = line.substr(tab + 1);
        hp.mods &= (MOD_ALT | MOD_CONTROL | MOD_SHIFT); // 归一化：丢弃杂位
        if (hp.vk && !hp.phrase.empty() && (hp.mods & (MOD_CONTROL | MOD_ALT)))
            out.push_back(std::move(hp));
        if (eol == std::wstring::npos) break;
    }
    return out;
}

bool SaveEntries(const std::vector<HotPhrase>& entries)
{
    std::wstring text;
    for (const auto& hp : entries)
    {
        wchar_t head[32] = {};
        _snwprintf_s(head, _TRUNCATE, L"%u,%u\t", hp.mods, hp.vk);
        text += head;
        text += hp.phrase;
        text += L"\r\n";
    }
    // 带 BOM 写：文本编辑器打开不乱码（解析端 BOM 容忍）
    std::string utf = "\xEF\xBB\xBF" + WToUtf8(text);
    return WriteFileBytes(HotPhrasePathW(), utf);
}

void RefreshHpList()
{
    if (!g_hHpList) return;
    auto entries = LoadEntries();
    SendMessageW(g_hHpList, LB_RESETCONTENT, 0, 0);
    for (const auto& hp : entries)
    {
        std::wstring line = FormatHotkey(hp.mods, hp.vk) + L"    " + hp.phrase;
        SendMessageW(g_hHpList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    wchar_t cap[96] = {};
    _snwprintf_s(cap, _TRUNCATE, L"共 %zu 条（最多 %zu；热键须含 Ctrl 或 Alt）",
                 entries.size(), kMaxHotPhrases);
    SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, cap);
}

// 热键录制框：KEYDOWN/SYSKEYDOWN 捕获（Ctrl/Alt/Shift 组合实时判定），
// ESC 清空；字符消息全部吞掉（框内不可编辑文本，只接受按键录制）。
LRESULT CALLBACK HotEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
    {
        if (wp == VK_ESCAPE || wp == VK_BACK)
        {
            g_capMods = 0;
            g_capVk = 0;
            SetWindowTextW(h, L"");
            return 0;
        }
        if (wp == VK_CONTROL || wp == VK_MENU || wp == VK_SHIFT || wp == VK_LWIN || wp == VK_RWIN)
            return 0; // 纯修饰键按下：等待主键
        uint32_t mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
        g_capMods = mods;
        g_capVk = (uint32_t)wp;
        SetWindowTextW(h, FormatHotkey(mods, (uint32_t)wp).c_str());
        return 0;
    }
    if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0; // 吞字符（含 Alt 蜂鸣）
    return CallWindowProcW(g_oldHotEditProc, h, msg, wp, lp);
}

void HpAdd()
{
    wchar_t phrase[1024] = {};
    GetDlgItemTextW(g_hHpWnd, IDC_HP_PHRASE, phrase, 1024);
    if (!g_capVk || !g_capMods)
    {
        SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"请先在上框中按下要录制的组合键（如 Ctrl+Alt+1）");
        return;
    }
    if (!(g_capMods & (MOD_CONTROL | MOD_ALT)))
    {
        SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"热键必须包含 Ctrl 或 Alt（否则会占用打字键）");
        return;
    }
    if (!phrase[0])
    {
        SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"请填写要输入的句式或句子");
        return;
    }
    auto entries = LoadEntries();
    if (entries.size() >= kMaxHotPhrases)
    {
        SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"已达上限，请先删除部分条目");
        return;
    }
    for (const auto& hp : entries)
        if (hp.mods == g_capMods && hp.vk == g_capVk)
        {
            SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"该组合键已被占用，请换一个");
            return;
        }
    HotPhrase hp{ g_capMods, g_capVk, phrase };
    entries.push_back(std::move(hp));
    if (!SaveEntries(entries))
    {
        MessageBoxW(g_hHpWnd, L"快捷短语写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    LX_LOG(L"ui", L"快捷短语添加: %ls", FormatHotkey(g_capMods, g_capVk).c_str());
    if (g_hpChanged) g_hpChanged();
    RefreshHpList();
    SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"已添加，热键即时生效");
}

void HpDeleteSelected()
{
    int sel = (int)SendMessageW(g_hHpList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"请先在列表中选择要删除的条目");
        return;
    }
    auto entries = LoadEntries();
    if (sel < (int)entries.size()) entries.erase(entries.begin() + sel);
    if (!SaveEntries(entries))
    {
        MessageBoxW(g_hHpWnd, L"快捷短语写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_hpChanged) g_hpChanged();
    RefreshHpList();
    SetDlgItemTextW(g_hHpWnd, IDC_HP_STATUS, L"已删除，热键已注销");
}

LRESULT CALLBACK HpWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_HP_ADD: HpAdd(); return 0;
        case IDC_HP_DEL: HpDeleteSelected(); return 0;
        case IDC_HP_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        // WinUI：窗体底中性灰 + 主文本色（与设置页一致）
        SetBkColor((HDC)wParam, kUiBg);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)HpBgBrush();
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

std::wstring FormatHotkey(uint32_t mods, uint32_t vk)
{
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    s += VkName(vk);
    return s;
}

std::vector<HotPhrase> LoadHotPhrases()
{
    return LoadEntries();
}

bool SaveHotPhrases(const std::vector<HotPhrase>& entries)
{
    return SaveEntries(entries);
}

void SetHotPhrasesChangedCallback(std::function<void()> cb)
{
    g_hpChanged = std::move(cb);
}

void ShowHotPhraseWindow(HINSTANCE hInst, HWND hParent)
{
    if (g_hHpWnd)
    {
        ShowWindow(g_hHpWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hHpWnd);
        RefreshHpList();
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = HpWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = HpBgBrush(); // WinUI 中性窗体底
    wc.lpszClassName = kHpWndClass;
    RegisterClassW(&wc);

    const int W = 620, H = 470, pad = 14, rowH = 30, btnW = 96;
    g_hHpWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kHpWndClass,
        L"快捷短语——自定义热键一键输入句式",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, W, H, hParent, NULL, hInst, NULL);

    HFONT font = HpFont();
    auto mkLabel = [&](const wchar_t* text, int x, int y, int w) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, w, 20, g_hHpWnd, NULL, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        return h;
    };

    int y = pad;
    mkLabel(L"已有快捷短语（点选后可删除）：", pad, y, W - pad * 2);
    y += 24;
    int listH = 180;
    g_hHpList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        pad, y, W - pad * 2 - 12, listH, g_hHpWnd, (HMENU)(INT_PTR)IDC_HP_LIST, hInst, NULL);
    SendMessageW(g_hHpList, WM_SETFONT, (WPARAM)font, TRUE);
    y += listH + 12;

    mkLabel(L"组合键（在框内按下，如 Ctrl+Alt+1；Esc 清空）：", pad, y, W - pad * 2);
    y += 22;
    HWND hHot = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        pad, y, 200, rowH, g_hHpWnd, (HMENU)(INT_PTR)IDC_HP_HOTEDIT, hInst, NULL);
    SendMessageW(hHot, WM_SETFONT, (WPARAM)font, TRUE);
    g_oldHotEditProc = (WNDPROC)SetWindowLongPtrW(hHot, GWLP_WNDPROC, (LONG_PTR)HotEditProc);

    HWND hPh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad + 212, y, W - pad * 2 - 212 - 12, rowH, g_hHpWnd, (HMENU)(INT_PTR)IDC_HP_PHRASE, hInst, NULL);
    SendMessageW(hPh, WM_SETFONT, (WPARAM)font, TRUE);
    y += rowH + 8;

    int bx = pad;
    auto mkBtn = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, y, btnW, rowH, g_hHpWnd, (HMENU)(INT_PTR)id, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        bx += btnW + 10;
        return h;
    };
    mkBtn(L"添加", IDC_HP_ADD);
    mkBtn(L"删除选中", IDC_HP_DEL);
    mkBtn(L"关闭", IDC_HP_CLOSE);
    y += rowH + 10;

    HWND hStat = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y, W - pad * 2, 22, g_hHpWnd, (HMENU)(INT_PTR)IDC_HP_STATUS, hInst, NULL);
    SendMessageW(hStat, WM_SETFONT, (WPARAM)font, TRUE);

    // 热键录制框初始聚焦（一打开即可按键录制）
    RefreshHpList();
    ShowWindow(g_hHpWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hHpWnd);
    SetFocus(hHot);
}

std::wstring HotPhraseDataPath()
{
    return HotPhrasePathW(); // 同 TU 匿名命名空间助手，此处可见
}

void NotifyHotPhrasesChanged()
{
    // 备份导入等外部改写存储文件后手动触发（SaveHotPhrases 自动触发；
    // 回调由 LingxiUI main 绑定为"注销→重读→重注册全部短语热键"）
    if (g_hpChanged) g_hpChanged();
}

} // namespace lingxi
