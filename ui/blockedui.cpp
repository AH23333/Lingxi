//
// R6 屏蔽词：存储 + 管理窗口（词输入框 + 列表 + 添加/删除/清空）。
// 模式沿用 phraseui.cpp（独立窗口、原子写、保存后 g_hooks 热载）。
// 引擎侧零改动即生效：blocked.txt 经 LoadUserDict 尾部 LoadBlockedLocked
// 与用户词典同通道重载，保存后 fire g_hooks.reloadUserDict。
// 口径镜像引擎 LoadBlockedLocked：词 1-50 字符、上限 500 条、BOM 容忍、去重。
//
#include <windows.h>

// 客户区补偿助手（QA 2026-11-02）：布局按客户区 W×H 设计，建窗经 AdjustWindowRectEx
// 补标题栏/边框——旧版拿窗口总高当客户区摆放，底部按钮行被裁 ≈21px（用户报
// "没有删除按钮"根因）。
static void AdjustWndRect(DWORD dwStyle, DWORD dwExStyle, int& w, int& h)
{
    RECT rc{ 0, 0, w, h };
    if (AdjustWindowRectEx(&rc, dwStyle, FALSE, dwExStyle))
    { w = rc.right - rc.left; h = rc.bottom - rc.top; }
}
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "blockedui.h"
#include "uiinternal.h" // g_hooks（P3 回调收口）
#include "../common/log.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kBlWndClass[] = L"LingxiBlockedWnd";

constexpr int IDC_BL_LIST = 600;
constexpr int IDC_BL_WORD = 601;
constexpr int IDC_BL_ADD = 602;
constexpr int IDC_BL_DEL = 603;
constexpr int IDC_BL_CLEAR = 604;
constexpr int IDC_BL_CLOSE = 605;
constexpr int IDC_BL_STATUS = 606;

// ---------------- 存储层 ----------------

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

// ReadFileBytes/WriteFileBytes：共享实现见 uiinternal.h（userdictwnd.cpp）

// ---------------- 管理窗口 ----------------

HWND g_hBlWnd = nullptr;
HWND g_hBlList = nullptr;
WNDPROC g_oldBlEditProc = nullptr;
WNDPROC g_oldBlListProc = nullptr;

// Esc 关窗（焦点在输入框/列表时子类化拦截；与"关闭"同语义=隐藏复用）
LRESULT CALLBACK BlEscEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        ShowWindow(GetParent(h), SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_oldBlEditProc, h, msg, wp, lp);
}

LRESULT CALLBACK BlEscListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        ShowWindow(GetParent(h), SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_oldBlListProc, h, msg, wp, lp);
}

HFONT BlFont()
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

HBRUSH BlBgBrush()
{
    static HBRUSH b = CreateSolidBrush(kUiBg);
    return b;
}

void RefreshBlList()
{
    if (!g_hBlList) return;
    auto words = LoadBlockedWords();
    SendMessageW(g_hBlList, LB_RESETCONTENT, 0, 0);
    for (const auto& w : words)
        SendMessageW(g_hBlList, LB_ADDSTRING, 0, (LPARAM)w.c_str());
    // 标题实时携带条目数：任务栏/窗口列表一眼可辨规模
    wchar_t title[96] = {};
    _snwprintf_s(title, _TRUNCATE,
                 L"屏蔽词——所列词永不进入候选（%zu 词）", words.size());
    SetWindowTextW(g_hBlWnd, title);
    wchar_t cap[128] = {};
    _snwprintf_s(cap, _TRUNCATE,
                 L"共 %zu 条（最多 %zu；词 1-50 字符，保存即热载、该词不再出现在候选）",
                 words.size(), kMaxBlockedEntries);
    SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, cap);
}

// 双击列表：词条回填输入框（便于改后重新添加）
void BlBackfillSelected()
{
    int sel = (int)SendMessageW(g_hBlList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    auto words = LoadBlockedWords();
    if (sel >= (int)words.size()) return;
    SetDlgItemTextW(g_hBlWnd, IDC_BL_WORD, words[sel].c_str());
    SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"已回填，可编辑后重新添加");
    SetFocus(GetDlgItem(g_hBlWnd, IDC_BL_WORD));
}

void BlAdd()
{
    wchar_t word[128] = {};
    GetDlgItemTextW(g_hBlWnd, IDC_BL_WORD, word, 128);
    std::wstring w = word;
    // 去首尾空白（词内空格保留——按原样整词匹配）
    size_t first = w.find_first_not_of(L" \t");
    w = (first == std::wstring::npos) ? L"" : w.substr(first);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    if (w.empty())
    {
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"请填写要屏蔽的词（1-50 字符）");
        return;
    }
    // 校验+去重+落盘+热载统一走公共 BlockWord（与候选右键屏蔽同一条路）
    switch (BlockWord(w))
    {
    case BlockWordResult::Blocked:
        RefreshBlList();
        SetDlgItemTextW(g_hBlWnd, IDC_BL_WORD, L"");
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"已添加，该词不会再出现在候选中");
        break;
    case BlockWordResult::AlreadyBlocked:
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"该词已在屏蔽列表中");
        break;
    case BlockWordResult::LimitReached:
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"已达上限，请先删除部分条目");
        break;
    case BlockWordResult::TooLong:
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"词过长（上限 50 字符）");
        break;
    case BlockWordResult::SaveFailed:
        MessageBoxW(g_hBlWnd, L"屏蔽词写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        break;
    }
}

void BlDeleteSelected()
{
    int sel = (int)SendMessageW(g_hBlList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"请先在列表中选择要删除的词");
        return;
    }
    auto words = LoadBlockedWords();
    // L8：越界=列表与文件已不同步——旧实现静默跳过删除仍报"已删除"，
    // 虚假成功；明示刷新重选
    if (sel >= (int)words.size())
    {
        SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"列表已变化，请刷新后重选");
        RefreshBlList();
        return;
    }
    words.erase(words.begin() + sel);
    if (!SaveBlockedWords(words))
    {
        MessageBoxW(g_hBlWnd, L"屏蔽词写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    LX_LOG(L"ui", L"屏蔽词删除（第 %d 条）", sel);
    if (g_hooks.reloadUserDict) g_hooks.reloadUserDict();
    RefreshBlList();
    SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"已删除，该词恢复参与候选");
}

void BlClearAll()
{
    if (MessageBoxW(g_hBlWnd, L"将清空全部屏蔽词（不可恢复），确定？",
                    L"清空屏蔽词", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    if (!SaveBlockedWords({}))
    {
        MessageBoxW(g_hBlWnd, L"屏蔽词写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    LX_LOG(L"ui", L"屏蔽词已清空");
    if (g_hooks.reloadUserDict) g_hooks.reloadUserDict();
    RefreshBlList();
    SetDlgItemTextW(g_hBlWnd, IDC_BL_STATUS, L"已清空，全部词恢复参与候选");
}

LRESULT CALLBACK BlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED)
            switch (LOWORD(wParam))
            {
            case IDC_BL_ADD: BlAdd(); return 0;
            case IDC_BL_DEL: BlDeleteSelected(); return 0;
            case IDC_BL_CLEAR: BlClearAll(); return 0;
            case IDC_BL_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
            }
        else if (HIWORD(wParam) == LBN_DBLCLK && LOWORD(wParam) == IDC_BL_LIST)
        {
            BlBackfillSelected(); // 双击条目回填输入框（改后重加）
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, kUiBg);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)BlBgBrush();
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ShowWindow(hwnd, SW_HIDE); // 窗体自身持焦点时 Esc 关窗
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

std::wstring BlockedWordsPathW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring dir = std::wstring(buf) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\blocked.txt";
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe) + L"blocked.txt";
}

std::vector<std::wstring> LoadBlockedWords()
{
    std::vector<std::wstring> out;
    std::string raw;
    if (!ReadFileBytes(BlockedWordsPathW(), raw)) return out;
    std::wstring text = Utf8ToW(raw);
    std::unordered_set<std::wstring> seen;
    size_t pos = 0;
    while (out.size() < kMaxBlockedEntries)
    {
        size_t eol = text.find(L'\n', pos);
        bool last = (eol == std::wstring::npos); // 先记 EOF，过滤分支不得跳过收尾 break
        std::wstring line = text.substr(pos,
            (last ? text.size() : eol) - pos);
        pos = last ? text.size() : eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line[0] == L'\xFEFF') line.erase(0, 1); // BOM
        if (!line.empty() && line.size() <= kMaxBlockedLen && seen.insert(line).second)
            out.push_back(line);  // 引擎同口径
        if (last) break;
    }
    return out;
}

bool SaveBlockedWords(const std::vector<std::wstring>& words)
{
    std::wstring text;
    size_t n = words.size() < kMaxBlockedEntries ? words.size() : kMaxBlockedEntries;
    for (size_t i = 0; i < n; i++)
    {
        if (words[i].empty() || words[i].size() > kMaxBlockedLen) continue;
        text += words[i];
        text += L"\r\n";
    }
    // 带 BOM 写：文本编辑器打开不乱码（引擎解析端 BOM 容忍）
    std::string utf = "\xEF\xBB\xBF" + WToUtf8(text);
    return WriteFileBytes(BlockedWordsPathW(), utf);
}

// ---------------- 右键屏蔽入口（公共 API） ----------------

bool IsWordBlocked(const std::wstring& word)
{
    if (word.empty()) return false;
    for (const auto& w : LoadBlockedWords())
        if (w == word) return true;
    return false;
}

BlockWordResult BlockWord(const std::wstring& word)
{
    if (word.empty() || word.size() > kMaxBlockedLen) return BlockWordResult::TooLong;
    auto words = LoadBlockedWords();
    for (const auto& w : words)
        if (w == word) return BlockWordResult::AlreadyBlocked;
    if (words.size() >= kMaxBlockedEntries) return BlockWordResult::LimitReached;
    words.push_back(word);
    if (!SaveBlockedWords(words)) return BlockWordResult::SaveFailed;
    LX_LOG(L"ui", L"屏蔽词添加: %ls", word.c_str());
    if (g_hooks.reloadUserDict) g_hooks.reloadUserDict();
    return BlockWordResult::Blocked;
}

bool UnblockWord(const std::wstring& word)
{
    auto words = LoadBlockedWords();
    bool found = false;
    for (size_t i = 0; i < words.size(); i++)
        if (words[i] == word)
        {
            words.erase(words.begin() + (long)i);
            found = true;
            break;
        }
    if (!found) return false;
    if (!SaveBlockedWords(words)) return false;
    LX_LOG(L"ui", L"屏蔽词移除: %ls", word.c_str());
    if (g_hooks.reloadUserDict) g_hooks.reloadUserDict();
    return true;
}

void ShowBlockedWindow(HINSTANCE hInst, HWND hParent)
{
    if (g_hBlWnd)
    {
        ShowWindow(g_hBlWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hBlWnd);
        RefreshBlList();
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = BlWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = BlBgBrush();
    wc.lpszClassName = kBlWndClass;
    RegisterClassW(&wc);

    const int W = 560, H = 450, pad = 14, rowH = 30, btnW = 96;
    const DWORD blStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int blW = W, blH = H;
    AdjustWndRect(blStyle, WS_EX_DLGMODALFRAME, blW, blH);
    g_hBlWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kBlWndClass,
        L"屏蔽词——所列词永不进入候选（敏感词、误学词等）",
        blStyle, CW_USEDEFAULT, CW_USEDEFAULT, blW, blH, hParent, NULL, hInst, NULL);

    HFONT font = BlFont();
    auto mkLabel = [&](const wchar_t* text, int x, int y, int w) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, w, 20, g_hBlWnd, NULL, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        return h;
    };

    int y = pad;
    mkLabel(L"已屏蔽的词（点选后可删除）：", pad, y, W - pad * 2);
    y += 24;
    int listH = 220;
    g_hBlList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        pad, y, W - pad * 2 - 12, listH, g_hBlWnd, (HMENU)(INT_PTR)IDC_BL_LIST, hInst, NULL);
    SendMessageW(g_hBlList, WM_SETFONT, (WPARAM)font, TRUE);
    g_oldBlListProc = (WNDPROC)SetWindowLongPtrW(g_hBlList, GWLP_WNDPROC, (LONG_PTR)BlEscListProc);
    y += listH + 12;

    mkLabel(L"要屏蔽的词（1-50 字符，如误学词、敏感词）：", pad, y, W - pad * 2);
    y += 22;
    HWND hWord = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad, y, W - pad * 2 - 12, rowH, g_hBlWnd, (HMENU)(INT_PTR)IDC_BL_WORD, hInst, NULL);
    SendMessageW(hWord, WM_SETFONT, (WPARAM)font, TRUE);
    g_oldBlEditProc = (WNDPROC)SetWindowLongPtrW(hWord, GWLP_WNDPROC, (LONG_PTR)BlEscEditProc);
    y += rowH + 8;

    int bx = pad;
    auto mkBtn = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, y, btnW, rowH, g_hBlWnd, (HMENU)(INT_PTR)id, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        bx += btnW + 10;
        return h;
    };
    mkBtn(L"添加", IDC_BL_ADD);
    mkBtn(L"删除选中", IDC_BL_DEL);
    mkBtn(L"清空", IDC_BL_CLEAR);
    mkBtn(L"关闭", IDC_BL_CLOSE);
    y += rowH + 10;

    HWND hStat = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y, W - pad * 2, 22, g_hBlWnd, (HMENU)(INT_PTR)IDC_BL_STATUS, hInst, NULL);
    SendMessageW(hStat, WM_SETFONT, (WPARAM)font, TRUE);

    RefreshBlList();
    ShowWindow(g_hBlWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hBlWnd);
    SetFocus(hWord);
}

} // namespace lingxi
