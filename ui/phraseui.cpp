//
// R2 自定义短语：存储 + 管理窗口（缩写 + 短语编辑框 + 列表 + 添加/删除）。
// 模式沿用 hotphraseui.cpp（独立窗口、原子写、保存后回调热载）。
// 引擎侧零改动即生效：phrases.txt 与 userdict 同通道 ReloadUserDict 重载。
//
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 客户区补偿助手（QA 2026-11-02）：布局代码按客户区 W×H 设计，建窗时经
// AdjustWindowRectEx 补齐标题栏/边框——旧版直接拿窗口总高当客户区摆放，
// 底部按钮行被非客户区裁掉 ≈21px 内容，即用户报"没有删除按钮"的裁切根因。
static void AdjustWndRect(DWORD dwStyle, DWORD dwExStyle, int& w, int& h)
{
    RECT rc{ 0, 0, w, h };
    if (AdjustWindowRectEx(&rc, dwStyle, FALSE, dwExStyle))
    { w = rc.right - rc.left; h = rc.bottom - rc.top; }
}
#include "phraseui.h"
#include "uiinternal.h" // g_hooks（P3 回调收口）
#include "../common/log.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kPhWndClass[] = L"LingxiPhraseWnd";

constexpr int IDC_PH_LIST = 500;
constexpr int IDC_PH_ABBREV = 501;
constexpr int IDC_PH_TEXT = 502;
constexpr int IDC_PH_ADD = 503;
constexpr int IDC_PH_DEL = 504;
constexpr int IDC_PH_CLOSE = 505;
constexpr int IDC_PH_STATUS = 506;

// ---------------- 存储层 ----------------

std::wstring PhrasePathW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring dir = std::wstring(buf) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\phrases.txt";
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe) + L"phrases.txt";
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

// ReadFileBytes/WriteFileBytes：共享实现见 uiinternal.h（userdictwnd.cpp）

// ---------------- 管理窗口 ----------------

HWND g_hPhWnd = nullptr;
HWND g_hPhList = nullptr;
WNDPROC g_oldPhEditProc = nullptr;
WNDPROC g_oldPhListProc = nullptr;

// Esc 关窗（焦点在输入框/列表时子类化拦截；与"关闭"同语义=隐藏复用）
LRESULT CALLBACK PhEscEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        ShowWindow(GetParent(h), SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_oldPhEditProc, h, msg, wp, lp);
}

LRESULT CALLBACK PhEscListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        ShowWindow(GetParent(h), SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_oldPhListProc, h, msg, wp, lp);
}

HFONT PhFont()
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

HBRUSH PhBgBrush()
{
    static HBRUSH b = CreateSolidBrush(kUiBg);
    return b;
}

// 缩写归一：去空白 + 小写化；返回 false = 含非字母或超限
bool NormalizeAbbrev(std::wstring& a)
{
    std::wstring out;
    for (wchar_t ch : a)
    {
        if (ch == L' ' || ch == L'\t') continue;
        if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch + 32);
        if (ch < L'a' || ch > L'z') return false;
        out.push_back(ch);
    }
    a = out;
    return a.size() >= 2 && a.size() <= kMaxAbbrevLen;
}

void RefreshPhList()
{
    if (!g_hPhList) return;
    auto entries = LoadPhraseEntries();
    SendMessageW(g_hPhList, LB_RESETCONTENT, 0, 0);
    for (const auto& e : entries)
    {
        std::wstring line = e.abbrev + L"    " + e.phrase;
        SendMessageW(g_hPhList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    // 标题实时携带条目数：任务栏/窗口列表一眼可辨规模
    wchar_t title[96] = {};
    _snwprintf_s(title, _TRUNCATE,
                 L"自定义短语——缩写一打就出长句（%zu 条）", entries.size());
    SetWindowTextW(g_hPhWnd, title);
    wchar_t cap[96] = {};
    _snwprintf_s(cap, _TRUNCATE, L"共 %zu 条（最多 %zu；缩写 2-16 位字母，整串打出即置顶）",
                 entries.size(), kMaxPhraseEntries);
    SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, cap);
}

// 双击列表：缩写+短语回填输入框（便于改后重新添加）
void PhBackfillSelected()
{
    int sel = (int)SendMessageW(g_hPhList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    auto entries = LoadPhraseEntries();
    if (sel >= (int)entries.size()) return;
    SetDlgItemTextW(g_hPhWnd, IDC_PH_ABBREV, entries[sel].abbrev.c_str());
    SetDlgItemTextW(g_hPhWnd, IDC_PH_TEXT, entries[sel].phrase.c_str());
    SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"已回填，可编辑后重新添加");
    SetFocus(GetDlgItem(g_hPhWnd, IDC_PH_TEXT));
}

void PhAdd()
{
    wchar_t abbrev[64] = {}, phrase[512] = {};
    GetDlgItemTextW(g_hPhWnd, IDC_PH_ABBREV, abbrev, 64);
    GetDlgItemTextW(g_hPhWnd, IDC_PH_TEXT, phrase, 512);
    std::wstring a = abbrev, t = phrase;
    // 短语去首尾空白与制表符（制表符是存储分隔符，混入会错位）
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    size_t first = t.find_first_not_of(L" \t");
    t = (first == std::wstring::npos) ? L"" : t.substr(first);
    if (!NormalizeAbbrev(a))
    {
        SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS,
                        L"缩写须为 2-16 位字母（a-z），不含数字/空格/符号");
        return;
    }
    if (t.empty() || t.size() > kMaxPhraseLen)
    {
        SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"请填写短语内容（1-200 字符）");
        return;
    }
    auto entries = LoadPhraseEntries();
    if (entries.size() >= kMaxPhraseEntries)
    {
        SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"已达上限，请先删除部分条目");
        return;
    }
    for (const auto& e : entries)
        if (e.abbrev == a && e.phrase == t)
        {
            SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"该缩写和短语已存在");
            return;
        }
    entries.push_back({ a, t });
    if (!SavePhraseEntries(entries))
    {
        MessageBoxW(g_hPhWnd, L"自定义短语写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    LX_LOG(L"ui", L"自定义短语添加: %ls -> %ls", a.c_str(), t.c_str());
    if (g_hooks.phrasesChanged) g_hooks.phrasesChanged();
    RefreshPhList();
    SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"已添加，打字时整串输入缩写即出");
}

void PhDeleteSelected()
{
    int sel = (int)SendMessageW(g_hPhList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"请先在列表中选择要删除的条目");
        return;
    }
    auto entries = LoadPhraseEntries();
    if (sel < (int)entries.size()) entries.erase(entries.begin() + sel);
    if (!SavePhraseEntries(entries))
    {
        MessageBoxW(g_hPhWnd, L"自定义短语写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_hooks.phrasesChanged) g_hooks.phrasesChanged();
    RefreshPhList();
    SetDlgItemTextW(g_hPhWnd, IDC_PH_STATUS, L"已删除，下次输入不再出现");
}

LRESULT CALLBACK PhWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED)
            switch (LOWORD(wParam))
            {
            case IDC_PH_ADD: PhAdd(); return 0;
            case IDC_PH_DEL: PhDeleteSelected(); return 0;
            case IDC_PH_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
            }
        else if (HIWORD(wParam) == LBN_DBLCLK && LOWORD(wParam) == IDC_PH_LIST)
        {
            PhBackfillSelected(); // 双击条目回填输入框（改后重加）
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, kUiBg);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)PhBgBrush();
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

std::vector<PhraseEntry> LoadPhraseEntries()
{
    std::vector<PhraseEntry> out;
    std::string raw;
    if (!ReadFileBytes(PhrasePathW(), raw)) return out;
    std::wstring text = Utf8ToW(raw);
    size_t pos = 0;
    while (out.size() < kMaxPhraseEntries)
    {
        size_t eol = text.find(L'\n', pos);
        bool last = (eol == std::wstring::npos); // 先记 EOF，过滤分支不得跳过收尾 break
        std::wstring line = text.substr(pos,
            (last ? text.size() : eol) - pos);
        pos = last ? text.size() : eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line[0] == L'\xFEFF') line.erase(0, 1); // BOM
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos)
        {
            PhraseEntry e;
            e.abbrev = line.substr(0, tab);
            e.phrase = line.substr(tab + 1);
            if (NormalizeAbbrev(e.abbrev) && !e.phrase.empty() && e.phrase.size() <= kMaxPhraseLen)
                out.push_back(std::move(e));
        }
        if (last) break;
    }
    return out;
}

bool SavePhraseEntries(const std::vector<PhraseEntry>& entries)
{
    std::wstring text;
    for (const auto& e : entries)
    {
        text += e.abbrev;
        text.push_back(L'\t');
        text += e.phrase;
        text += L"\r\n";
    }
    // 带 BOM 写：文本编辑器打开不乱码（引擎解析端 BOM 容忍）
    std::string utf = "\xEF\xBB\xBF" + WToUtf8(text);
    return WriteFileBytes(PhrasePathW(), utf);
}

void ShowPhraseWindow(HINSTANCE hInst, HWND hParent)
{
    if (g_hPhWnd)
    {
        ShowWindow(g_hPhWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hPhWnd);
        RefreshPhList();
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = PhWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = PhBgBrush();
    wc.lpszClassName = kPhWndClass;
    RegisterClassW(&wc);

    const int W = 620, H = 470, pad = 14, rowH = 30, btnW = 96;
    const DWORD phStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int phW = W, phH = H;
    AdjustWndRect(phStyle, WS_EX_DLGMODALFRAME, phW, phH);
    g_hPhWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPhWndClass,
        L"自定义短语——缩写一打就出长句（如 addr→地址）",
        phStyle, CW_USEDEFAULT, CW_USEDEFAULT, phW, phH, hParent, NULL, hInst, NULL);

    HFONT font = PhFont();
    auto mkLabel = [&](const wchar_t* text, int x, int y, int w) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, w, 20, g_hPhWnd, NULL, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        return h;
    };

    int y = pad;
    mkLabel(L"已有自定义短语（点选后可删除）：", pad, y, W - pad * 2);
    y += 24;
    int listH = 180;
    g_hPhList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        pad, y, W - pad * 2 - 12, listH, g_hPhWnd, (HMENU)(INT_PTR)IDC_PH_LIST, hInst, NULL);
    SendMessageW(g_hPhList, WM_SETFONT, (WPARAM)font, TRUE);
    g_oldPhListProc = (WNDPROC)SetWindowLongPtrW(g_hPhList, GWLP_WNDPROC, (LONG_PTR)PhEscListProc);
    y += listH + 12;

    mkLabel(L"缩写（2-16 位字母）：", pad, y, 200);
    mkLabel(L"短语内容（打出缩写后置顶候选）：", pad + 212, y, W - pad * 2 - 212);
    y += 22;
    HWND hAb = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad, y, 200, rowH, g_hPhWnd, (HMENU)(INT_PTR)IDC_PH_ABBREV, hInst, NULL);
    SendMessageW(hAb, WM_SETFONT, (WPARAM)font, TRUE);
    HWND hTx = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad + 212, y, W - pad * 2 - 212 - 12, rowH, g_hPhWnd, (HMENU)(INT_PTR)IDC_PH_TEXT, hInst, NULL);
    SendMessageW(hTx, WM_SETFONT, (WPARAM)font, TRUE);
    // 两个 EDIT 同类共享原类过程，同一份 g_oldPhEditProc
    g_oldPhEditProc = (WNDPROC)SetWindowLongPtrW(hAb, GWLP_WNDPROC, (LONG_PTR)PhEscEditProc);
    SetWindowLongPtrW(hTx, GWLP_WNDPROC, (LONG_PTR)PhEscEditProc);
    y += rowH + 8;

    int bx = pad;
    auto mkBtn = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, y, btnW, rowH, g_hPhWnd, (HMENU)(INT_PTR)id, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        bx += btnW + 10;
        return h;
    };
    mkBtn(L"添加", IDC_PH_ADD);
    mkBtn(L"删除选中", IDC_PH_DEL);
    mkBtn(L"关闭", IDC_PH_CLOSE);
    y += rowH + 10;

    HWND hStat = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y, W - pad * 2, 22, g_hPhWnd, (HMENU)(INT_PTR)IDC_PH_STATUS, hInst, NULL);
    SendMessageW(hStat, WM_SETFONT, (WPARAM)font, TRUE);

    RefreshPhList();
    ShowWindow(g_hPhWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hPhWnd);
    SetFocus(hAb);
}

} // namespace lingxi
