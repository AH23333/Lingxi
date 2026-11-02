//
// 用户词典：存储层 + 管理窗 + 候选词右键词典操作。
// 原 settingsui.cpp 词典段拆出（P1 重构）。
// - 存储：%APPDATA%\LingxiIme\userdict.txt（UTF-8：词\t拼音\t次数）；
//   单写者模型（2026-11-01"删除复活"根治）：唯一写者=拼音引擎（持锁改
//   内存+落盘等收敛）；本窗全部管理操作经 g_hooks 引擎原子操作直达，
//   不再直写词典文件（旧模型后台在途旧快照会把已删词条写回复活）。
// - 管理窗：搜索过滤（词条/拼音子串实时过滤）+ 删/清/导/导入 + 自定义
//   词条（拼音留空自动推断）+ 提升/降低排序。
// - 右键操作（window.cpp 候选窗右键菜单）：置顶/提升/降频/删除/查拼音。
//
#include "uiinternal.h"
// 客户区补偿助手（QA 2026-11-02）：布局代码按客户区 W×H 设计，建窗时经
// AdjustWindowRectEx 补齐标题栏/边框——旧版直接拿窗口总高当客户区摆放，
// 底部按钮行被非客户区裁掉 ≈21px 内容，即用户报"没有删除按钮"的裁切根因。
static void AdjustWndRect(DWORD dwStyle, DWORD dwExStyle, int& w, int& h)
{
    RECT rc{ 0, 0, w, h };
    if (AdjustWindowRectEx(&rc, dwStyle, FALSE, dwExStyle))
    { w = rc.right - rc.left; h = rc.bottom - rc.top; }
}
#include "settingsui.h" // CandDictOp/UserDictHasWord/ApplyCandidateDictOp/InferWordPinyin 公共声明

#include <windows.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <unordered_map>

#include "../common/log.h"
#include "../common/settings.h"
#include "blockedui.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kUserDictWndClass[] = L"LingxiUserDictWnd";

// ---------------- 文件与编码工具 ----------------

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 解析用户词典文本为条目（兼容 UTF-8 BOM）。
std::vector<UserDictEntry> ParseUserDict(const std::string& raw)
{
    std::string text = raw;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    std::vector<UserDictEntry> v;
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
        UserDictEntry e;
        e.word = Utf8ToW(line.substr(0, t1));
        e.key = Utf8ToW(line.substr(t1 + 1, t2 - t1 - 1));
        e.count = atoi(line.substr(t2 + 1).c_str());
        if (!e.word.empty() && !e.key.empty() && e.count > 0) v.push_back(std::move(e));
    }
    return v;
}

// 拼音推断：zh_pinyin.txt（data\，词条\t空格分隔读音）懒加载缓存。
// 整词精确命中优先；否则逐字取单字条目首见读音连写。推断失败返回空串
// （此时要求用户手填拼音）。键口径与引擎一致：小写、无空格/撇号连写。
struct PinyinInfer
{
    std::unordered_map<std::wstring, std::wstring> phrase; // 多字词→连写键
    std::unordered_map<wchar_t, std::wstring> single;      // 单字→首选读音
};

const PinyinInfer& GetPinyinInfer()
{
    static PinyinInfer inf = [] {
        PinyinInfer r;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) slash[1] = 0;
        std::string raw;
        if (!ReadFileBytes(std::wstring(exe) + L"data\\zh_pinyin.txt", raw)) return r;
        std::wstring text = Utf8ToW(raw);
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = text.size();
            std::wstring line = text.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos) continue;
            std::wstring word = line.substr(0, tab);
            std::wstring key;
            for (wchar_t ch : line.substr(tab + 1))
                if (ch != L' ' && ch != L'\'') key.push_back((wchar_t)towlower(ch));
            if (word.empty() || key.empty()) continue;
            if (word.size() == 1)
            {
                if (!r.single.count(word[0])) r.single.emplace(word[0], key);
            }
            else
                r.phrase.emplace(std::move(word), std::move(key));
        }
        return r;
    }();
    return inf;
}

std::wstring InferPinyinKey(const std::wstring& word)
{
    const PinyinInfer& inf = GetPinyinInfer();
    auto it = inf.phrase.find(word);
    if (it != inf.phrase.end()) return it->second;
    std::wstring key;
    for (wchar_t ch : word)
    {
        auto sit = inf.single.find(ch);
        if (sit == inf.single.end()) return L"";
        key += sit->second;
    }
    return key;
}

// ---------------- 用户词典管理窗口 ----------------

constexpr int IDC_UD_LIST = 300;
constexpr int IDC_UD_STATIC = 301;
constexpr int IDC_UD_DEL = 302;
constexpr int IDC_UD_CLEAR = 303;
constexpr int IDC_UD_IMPORT = 304;
constexpr int IDC_UD_EXPORT = 305;
constexpr int IDC_UD_CLOSE = 306;
constexpr int IDC_UD_WORD = 307;  // 自定义词条输入框
constexpr int IDC_UD_KEY = 308;   // 拼音输入框（空=自动推断）
constexpr int IDC_UD_ADD = 309;   // 添加/已存在则提升
constexpr int IDC_UD_BOOST = 310; // 提升选中（+1000，排到更前）
constexpr int IDC_UD_LOWER = 311; // 降低选中（-1000，下限 1）
constexpr int IDC_UD_SEARCH = 312; // 搜索过滤框（词条/拼音子串，不区分大小写）
constexpr int IDC_UD_BLOCKED = 313; // R6 屏蔽词管理窗入口

HWND g_hUserDict = nullptr;
HWND g_hUdList = nullptr;
HWND g_hUdSearch = nullptr;
WNDPROC g_udListOrig = nullptr; // 列表框子类化前原 proc

// 列表框子类化：Delete 键删除选中（转发与"删除选中"按钮同一命令路径）
LRESULT CALLBACK UdListProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_DELETE)
    {
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDC_UD_DEL, 0), 0);
        return 0;
    }
    return CallWindowProcW(g_udListOrig, h, msg, wParam, lParam);
}

void RefreshUdList()
{
    if (!g_hUdList) return;
    // 搜索过滤：词条或拼音键的子串命中（不区分大小写）；空=全量
    std::wstring fl;
    if (g_hUdSearch)
    {
        wchar_t filter[64] = L"";
        GetWindowTextW(g_hUdSearch, filter, 64);
        for (wchar_t ch : std::wstring(filter))
            fl.push_back((wchar_t)towlower(ch));
    }
    std::string raw;
    std::vector<UserDictEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    SendMessageW(g_hUdList, LB_RESETCONTENT, 0, 0);
    size_t shown = 0;
    for (size_t i = 0; i < entries.size(); i++)
    {
        if (!fl.empty())
        {
            std::wstring w = entries[i].word, k = entries[i].key;
            for (auto& ch : w) ch = (wchar_t)towlower(ch);
            for (auto& ch : k) ch = (wchar_t)towlower(ch);
            if (w.find(fl) == std::wstring::npos && k.find(fl) == std::wstring::npos)
                continue;
        }
        wchar_t line[256];
        _snwprintf_s(line, _TRUNCATE, L"%ls    %ls    %d",
                     entries[i].word.c_str(), entries[i].key.c_str(), entries[i].count);
        SendMessageW(g_hUdList, LB_ADDSTRING, 0, (LPARAM)line);
        shown++;
    }
    wchar_t cap[96];
    if (fl.empty())
        _snwprintf_s(cap, _TRUNCATE, L"已学习词条：%zu", entries.size());
    else
        _snwprintf_s(cap, _TRUNCATE, L"匹配 %zu / %zu 条（清空搜索框恢复全量）",
                     shown, entries.size());
    SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, cap);
}

void UdDeleteSelected()
{
    int sel = (int)SendMessageW(g_hUdList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"请先在列表中选择词条");
        return;
    }
    // 按行内容取 (word,key)：引擎自学习随时重写词典，列表索引与文件会漂移
    // （按索引删会删错/落空）。行格式 "word    key    count"（4 空格分隔）。
    wchar_t line[256] = L"";
    SendMessageW(g_hUdList, LB_GETTEXT, sel, (LPARAM)line);
    std::wstring word, key;
    {
        const wchar_t* sep1 = wcsstr(line, L"    ");
        if (sep1)
        {
            word.assign(line, sep1 - line);
            const wchar_t* rest = sep1 + 4;
            const wchar_t* sep2 = wcsstr(rest, L"    ");
            if (sep2)
                key.assign(rest, sep2 - rest);
        }
    }
    if (word.empty() || key.empty())
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"该词条已不存在（可能刚被学习进程更新），列表已刷新");
        RefreshUdList();
        return;
    }
    // 删除直达引擎原子操作（单写者落盘，无在途旧快照复活）
    if (!g_hooks.removeUserWord || !g_hooks.removeUserWord(word, key))
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"该词条已不存在（可能刚被学习进程更新），列表已刷新");
        RefreshUdList();
        return;
    }
    RefreshUdList();
    int n = (int)SendMessageW(g_hUdList, LB_GETCOUNT, 0, 0);
    if (n > 0) SendMessageW(g_hUdList, LB_SETCURSEL, sel < n ? sel : n - 1, 0);
    SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC,
        L"已删除并重新加载（基础词典里的常见词仍会出现在候选中）");
    LX_LOG(L"ui", L"用户词典删除: %ls (key=%ls)", word.c_str(), key.c_str());
}

void UdClearAll()
{
    if (MessageBoxW(g_hUserDict, L"确定清空全部已学习词条？此操作不可撤销。",
                    L"清空用户词典", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    // 清空直达引擎原子操作（不再直写文件——防在途旧快照复活）
    if (!g_hooks.clearUserDict || !g_hooks.clearUserDict())
    {
        MessageBoxW(g_hUserDict, L"用户词典清空失败（引擎未运行或写入失败）。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
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

    std::string inRaw;
    if (!ReadFileBytes(path, inRaw)) return;
    std::vector<UserDictEntry> imported = ParseUserDict(inRaw);
    if (imported.empty())
    {
        MessageBoxW(g_hUserDict, L"文件中没有可导入的词条（每行需为：词、拼音、次数，以制表符分隔）。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    // 合并直达引擎原子操作：同 (word,key) 累加 count，否则追加
    if (!g_hooks.mergeUserDict || !g_hooks.mergeUserDict(imported))
    {
        MessageBoxW(g_hUserDict, L"导入失败（引擎未运行或写入失败），未合并词条。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    RefreshUdList();
}

void UdExport()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = g_hUserDict;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::string raw;
    if (!ReadFileBytes(UserDictPathW(), raw) || !WriteFileBytes(path, raw))
    {
        // 与设置备份同口径（审计轮 7）：导出失败必须明示，
        // 防“我以为备份了”的虚假信心。
        MessageBoxW(g_hUserDict, L"导出失败（词典读取失败或目标位置不可写），未生成备份文件。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
}

// 自定义词条：手动添加候选词（词+拼音，拼音留空自动推断）。
// 新词条 count=1000（进入引擎置顶区，按次数排位）；同词同键已存在 → +1000 提升。
void UdAddWord()
{
    wchar_t word[128] = {}, key[256] = {};
    GetDlgItemTextW(g_hUserDict, IDC_UD_WORD, word, 128);
    GetDlgItemTextW(g_hUserDict, IDC_UD_KEY, key, 256);
    std::wstring w = word;
    std::wstring k;
    for (wchar_t ch : std::wstring(key))
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))
            k.push_back((wchar_t)towlower(ch));
    if (w.empty())
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"请填写词条（如：灵犀输入法）");
        return;
    }
    if (k.empty())
        k = InferPinyinKey(w);
    if (k.empty())
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC,
                        L"无法自动推断该词拼音（含词典外字符），请手动填写拼音");
        return;
    }
    bool merged = UserDictHasWord(w); // 预读：同词已存在 → 状态文案走提升语义
    // 新增/提升直达引擎原子操作（count=1000 进置顶区）
    if (!g_hooks.addUserWord || !g_hooks.addUserWord(w, k, 1000))
    {
        MessageBoxW(g_hUserDict, L"词条保存失败（引擎未运行或写入失败）。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    RefreshUdList();
    SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC,
        merged ? L"词条已存在，已提升其排序（+1000）"
               : L"已添加：输入对应拼音时该词将固定出现在候选置顶区");
    SetDlgItemTextW(g_hUserDict, IDC_UD_WORD, L"");
    SetDlgItemTextW(g_hUserDict, IDC_UD_KEY, L"");
    LX_LOG(L"ui", L"自定义词条添加: %ls (key=%ls merged=%d)", w.c_str(), k.c_str(), (int)merged);
}

// 提升/降低选中词条排序（改 count；提升=+1000 排更前，降低=-1000 下限 1）
void UdNudgeSelected(int delta)
{
    int sel = (int)SendMessageW(g_hUdList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"请先在列表中选择词条");
        return;
    }
    // 同 UdDeleteSelected：按行内容取 (word,key)（引擎并发重写词典，索引漂移）
    wchar_t line[256] = L"";
    SendMessageW(g_hUdList, LB_GETTEXT, sel, (LPARAM)line);
    std::wstring word, key;
    {
        const wchar_t* sep1 = wcsstr(line, L"    ");
        if (sep1)
        {
            word.assign(line, sep1 - line);
            const wchar_t* rest = sep1 + 4;
            const wchar_t* sep2 = wcsstr(rest, L"    ");
            if (sep2)
                key.assign(rest, sep2 - rest);
        }
    }
    // 调频直达引擎原子操作（key 空=该词全部读音一起调；下限 1 引擎侧保证）
    if (word.empty() || key.empty() ||
        !g_hooks.nudgeUserWord || !g_hooks.nudgeUserWord(word, key, delta))
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"该词条已不存在（可能刚被学习进程更新），列表已刷新");
        RefreshUdList();
        return;
    }
    RefreshUdList();
    // 按内容重选（索引可能因 count 排序变化而移动）
    for (int i = 0; i < (int)SendMessageW(g_hUdList, LB_GETCOUNT, 0, 0); i++)
    {
        wchar_t l2[256] = L"";
        SendMessageW(g_hUdList, LB_GETTEXT, i, (LPARAM)l2);
        if (wcsstr(l2, word.c_str()) == l2)
        {
            SendMessageW(g_hUdList, LB_SETCURSEL, i, 0);
            break;
        }
    }
    SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC,
        delta > 0 ? L"已提升排序（+1000）" : L"已降低排序（-1000，下限 1）");
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
        case IDC_UD_ADD: UdAddWord(); return 0;
        case IDC_UD_BOOST: UdNudgeSelected(1000); return 0;
        case IDC_UD_LOWER: UdNudgeSelected(-1000); return 0;
        case IDC_UD_BLOCKED: // R6：词典窗一键直达屏蔽词管理（同 blocked.txt 通道）
            ShowBlockedWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), hwnd);
            return 0;
        case IDC_UD_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE) RefreshUdList(); // 实时过滤
            return 0;
        case IDC_UD_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        // WinUI：窗体底中性灰 + 主文本色（与设置页一致）
        SetBkColor((HDC)wParam, kUiBg);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hBgBrush;
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

// ---------------- 存储路径与原子读写（跨文件共享：备份/托盘/统计导出）----------------

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

bool UserDictReplaceFromRaw(const std::string& rawUtf8)
{
    // 备份恢复：清空+导入两条引擎原子操作（不再直写词典文件——与后台落盘
    // 竞态会在途旧快照复活旧词条，且绕过引擎内存态）
    if (!g_hooks.clearUserDict || !g_hooks.clearUserDict())
        return false;
    std::vector<UserDictEntry> entries = ParseUserDict(rawUtf8);
    if (entries.empty()) return true;
    return g_hooks.mergeUserDict && g_hooks.mergeUserDict(entries);
}

void CreateUserDictWindow(HINSTANCE hInst)
{
    if (g_hUserDict) { ShowWindow(g_hUserDict, SW_SHOW); SetForegroundWindow(g_hUserDict); RefreshUdList(); return; }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = UserDictWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!g_hBgBrush)
        g_hBgBrush = CreateSolidBrush(kUiBg);
    wc.hbrBackground = g_hBgBrush; // WinUI 中性窗体底
    wc.lpszClassName = kUserDictWndClass;
    RegisterClassExW(&wc);

    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int W = MulDiv(520, dpi, 96), H = MulDiv(480, dpi, 96);
    int btnH = MulDiv(28, dpi, 96);
    int pad = MulDiv(12, dpi, 96);

    const DWORD udStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int udW = W, udH = H;
    AdjustWndRect(udStyle, 0, udW, udH);   // 补齐非客户区，保证客户区=W×H
    g_hUserDict = CreateWindowExW(0, kUserDictWndClass, L"用户词典与自定义词条管理",
                                  udStyle, CW_USEDEFAULT, CW_USEDEFAULT, udW, udH, NULL, NULL, hInst, NULL);

    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                    pad, pad, W - pad * 2, btnH, g_hUserDict, (HMENU)(INT_PTR)IDC_UD_STATIC, hInst, NULL);
    // 搜索过滤行（词条/拼音子串实时过滤；清空恢复全量）
    g_hUdSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  pad, pad + btnH + 4, W - pad * 2, btnH, g_hUserDict,
                                  (HMENU)(INT_PTR)IDC_UD_SEARCH, hInst, NULL);
    SendMessageW(g_hUdSearch, 0x1501 /*EM_SETCUEBANNERW*/, TRUE,
                 (LPARAM)L"搜索：词条或拼音（实时过滤）");
    // 自底向上：按钮行 → 自定义词条输入行 → 列表占满余下空间
    int rowBtnY = H - btnH - pad;
    int inputY = rowBtnY - btnH - 6;
    int listTop = pad + btnH * 2 + 8;
    g_hUdList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                pad, listTop, W - pad * 2, inputY - listTop - 8,
                                g_hUserDict, (HMENU)(INT_PTR)IDC_UD_LIST, hInst, NULL);
    // Delete 键删除选中（子类化转发，见 UdListProc）
    g_udListOrig = (WNDPROC)SetWindowLongPtrW(g_hUdList, GWLP_WNDPROC, (LONG_PTR)UdListProc);

    // 自定义词条输入行：词条 + 拼音（留空自动推断）+ 添加（已存在则提升排序）
    HWND hWord = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 pad, inputY, MulDiv(160, dpi, 96), btnH, g_hUserDict,
                                 (HMENU)(INT_PTR)IDC_UD_WORD, hInst, NULL);
    SendMessageW(hWord, 0x1501 /*EM_SETCUEBANNERW*/, TRUE, (LPARAM)L"自定义词条，如：灵犀输入法");
    HWND hKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                pad + MulDiv(168, dpi, 96), inputY, MulDiv(180, dpi, 96), btnH,
                                g_hUserDict, (HMENU)(INT_PTR)IDC_UD_KEY, hInst, NULL);
    SendMessageW(hKey, 0x1501 /*EM_SETCUEBANNERW*/, TRUE, (LPARAM)L"拼音，留空自动推断");
    CreateWindowExW(0, L"BUTTON", L"添加",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    pad + MulDiv(356, dpi, 96), inputY, MulDiv(80, dpi, 96), btnH, g_hUserDict,
                    (HMENU)(INT_PTR)IDC_UD_ADD, hInst, NULL);

    struct Btn { const wchar_t* text; int id; };
    Btn btns[] = {
        { L"提升选中", IDC_UD_BOOST }, { L"降低选中", IDC_UD_LOWER },
        { L"删除选中", IDC_UD_DEL }, { L"清空", IDC_UD_CLEAR },
        { L"导入", IDC_UD_IMPORT }, { L"导出", IDC_UD_EXPORT },
        { L"屏蔽词", IDC_UD_BLOCKED }, { L"关闭", IDC_UD_CLOSE },
    };
    int bw = (W - pad * 2 - 7 * 5) / 8;
    for (int i = 0; i < 8; i++)
    {
        int bx = pad + i * (bw + 5);
        CreateWindowExW(0, L"BUTTON", btns[i].text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        bx, rowBtnY, bw, btnH, g_hUserDict,
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

// ---------------- 候选词右键词典操作（候选窗右键菜单，2026-10-29） ----------------
// 公共 API（settingsui.h），window.cpp 引用。

bool UserDictHasWord(const std::wstring& word)
{
    if (word.empty()) return false;
    std::string raw;
    if (!ReadFileBytes(UserDictPathW(), raw)) return false;
    for (const auto& e : ParseUserDict(raw))
        if (e.word == word) return true;
    return false;
}

bool ApplyCandidateDictOp(const std::wstring& word, const std::wstring& keyHint,
                          CandDictOp op)
{
    if (word.empty()) return false;
    // 新词条键推导：拼音推断优先（小写全拼连写，与引擎 NormKey 口径一致，
    // 与用户用什么输入方式打的无关）；推断失败回退 keyHint（当前组合串）——
    // 仅全拼模式可用，双拼下组合串是键位序列而非全拼（引擎 Learn 同因先转换）。
    auto deriveKey = [&]() -> std::wstring
    {
        std::wstring k = InferPinyinKey(word);
        if (!k.empty()) return k;
        if (LoadSettings().shuangpin) return L"";
        for (wchar_t ch : keyHint)
        {
            if (ch >= L'a' && ch <= L'z') k.push_back(ch);
            else if (ch >= L'A' && ch <= L'Z') k.push_back((wchar_t)towlower(ch));
        }
        return k;
    };

    // 全部走引擎原子操作（单写者落盘；key 空=该词全部读音）
    bool ok = false;
    if (op == CandDictOp::Delete)
    {
        ok = g_hooks.removeUserWord && g_hooks.removeUserWord(word, L"");
        if (!ok)
            LX_LOG(L"ui", L"右键删除落空（基础词典词或引擎失败）: %ls", word.c_str());
    }
    else if (op == CandDictOp::Lower)
    {
        ok = g_hooks.nudgeUserWord && g_hooks.nudgeUserWord(word, L"", -1000);
        if (!ok)
            LX_LOG(L"ui", L"右键降频落空（词条不在用户词典）: %ls", word.c_str());
    }
    else
    {
        int delta = (op == CandDictOp::Pin) ? 10000 : 1000;
        // 已有词条直接调频；不在词典则按推导键新增
        if (g_hooks.nudgeUserWord && g_hooks.nudgeUserWord(word, L"", delta))
            ok = true;
        else
        {
            std::wstring k = deriveKey();
            if (k.empty())
                LX_LOG(L"ui", L"右键新增落空（无法确定拼音键）: %ls", word.c_str());
            else
                ok = g_hooks.addUserWord && g_hooks.addUserWord(word, k, delta);
        }
    }
    LX_LOG(L"ui", L"右键词典操作: op=%d word=%ls ok=%d", (int)op, word.c_str(), (int)ok);
    return ok;
}

std::wstring InferWordPinyin(const std::wstring& word)
{
    // 匿名命名空间助手 InferPinyinKey 的导出包装（候选窗右键"查拼音"用）
    return InferPinyinKey(word);
}

} // namespace lingxi
