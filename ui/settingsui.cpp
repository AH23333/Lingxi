//
// 托盘图标 + 设置窗口。
//
// 设置存 HKCU\Software\LingxiIme（common/settings），DLL 与 UI 每次读取实时生效：
// - 拼音组合模式：DLL 在模式检测（焦点/激活/语言变化）时读取
// - 候选数量 / 界面明暗 / 英文逐键补全：UI 在每次预测请求时读取
//
#include "settingsui.h"
#include "hotphraseui.h"

#include <shellapi.h>
#include <commdlg.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

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


constexpr int IDC_CHK_PINYIN = 101;
constexpr int IDC_CHK_EN = 102;
constexpr int IDC_EDT_COUNT = 104;
constexpr int IDC_CMB_FONTSIZE = 105;
constexpr int IDC_CMB_DARK = 106;
constexpr int IDC_CHK_CNPUNCT = 107;
constexpr int IDC_CHK_FUZZY = 108;
constexpr int IDC_CMB_TOGGLEMODE = 117; // B5 草案②：中英切换键（旧 109 复选框弃用）
constexpr int IDC_CHK_VMODE = 110;
constexpr int IDC_BTN_USERDICT = 111;
constexpr int IDC_CHK_SHUANGPIN = 112;
constexpr int IDC_CHK_SYMBOLHOTKEY = 113;
constexpr int IDC_CHK_TRADITIONAL = 114;
constexpr int IDC_CMB_SKIN = 115;
constexpr int IDC_CHK_LLMRERANK = 116;
constexpr int IDC_BTN_HOTPHRASES = 118; // 快捷短语管理入口

HWND g_hMain = nullptr;    // 隐藏消息窗口（托盘宿主）
HWND g_hSettings = nullptr; // 设置窗口（关闭=隐藏，常驻）
HWND g_hChkPinyin = nullptr, g_hChkEn = nullptr, g_hEdtCount = nullptr,
     g_hCmbDark = nullptr, g_hCmbFontSize = nullptr, g_hCmbSkin = nullptr,
     g_hCmbToggleMode = nullptr,
     g_hChkCnPunct = nullptr, g_hChkFuzzy = nullptr,
     g_hChkVMode = nullptr,
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
    ComboBox_SetCurSel(g_hCmbToggleMode, (int)(s.toggleMode <= 4 ? s.toggleMode : 1));
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
    int tsel = ComboBox_GetCurSel(g_hCmbToggleMode);
        if (tsel >= 0 && tsel <= 4) s.toggleMode = (uint32_t)tsel;
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
    LX_LOG(L"ui", L"设置已保存: pinyin=%d count=%u dark=%u en=%d cnPunct=%d fuzzy=%d toggleMode=%u v=%d",
           (int)s.pinyinEnabled, s.candidateCount, s.darkMode, (int)s.enCompletion,
           (int)s.cnPunct, (int)s.fuzzyPinyin, (unsigned)s.toggleMode, (int)s.vMode);
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
    if (ReadFileBytes(UserDictPathW(), raw)) WriteFileBytes(path, raw);
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
    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    bool merged = false;
    for (auto& e : entries)
        if (e.word == w && e.key == k) { e.count += 1000; merged = true; break; }
    if (!merged) entries.push_back({ w, k, 1000 });
    if (!WriteFileBytes(UserDictPathW(), SerializeUserDict(entries)))
    {
        MessageBoxW(g_hUserDict, L"用户词典写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_reloadUserDict) g_reloadUserDict();
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
    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    if (sel >= (int)entries.size()) return;
    entries[sel].count += delta;
    if (entries[sel].count < 1) entries[sel].count = 1;
    if (!WriteFileBytes(UserDictPathW(), SerializeUserDict(entries)))
    {
        MessageBoxW(g_hUserDict, L"用户词典写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_reloadUserDict) g_reloadUserDict();
    RefreshUdList();
    SendMessageW(g_hUdList, LB_SETCURSEL, sel, 0);
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
    int W = MulDiv(520, dpi, 96), H = MulDiv(480, dpi, 96);
    int btnH = MulDiv(28, dpi, 96);
    int pad = MulDiv(12, dpi, 96);

    g_hUserDict = CreateWindowExW(0, kUserDictWndClass, L"用户词典与自定义词条管理",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, hInst, NULL);

    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                    pad, pad, W - pad * 2, btnH, g_hUserDict, (HMENU)(INT_PTR)IDC_UD_STATIC, hInst, NULL);
    // 自底向上：按钮行 → 自定义词条输入行 → 列表占满余下空间
    int rowBtnY = H - btnH - pad;
    int inputY = rowBtnY - btnH - 6;
    int listTop = pad + btnH + 4;
    g_hUdList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                pad, listTop, W - pad * 2, inputY - listTop - 8,
                                g_hUserDict, (HMENU)(INT_PTR)IDC_UD_LIST, hInst, NULL);

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
        { L"关闭", IDC_UD_CLOSE },
    };
    int bw = (W - pad * 2 - 6 * 6) / 7;
    for (int i = 0; i < 7; i++)
    {
        int bx = pad + i * (bw + 6);
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

// ---------------- 设置窗口 ----------------

// 流式重排布局：控件注册进 g_layout（WRAP 可换行并排 / SECTION 段标题独行
// / BOTTOM 锚底），LayoutSettings() 按客户区宽度流式排布——窗口加宽自动
// 折成多列，拖拽缩放（WS_THICKFRAME）有实际意义。
namespace {

enum LayoutFlags
{
    IT_WRAP = 1,    // 可与相邻项并排，放不下换行
    IT_SECTION = 2, // 段标题：强制新行
    IT_BOTTOM = 8,  // 锚定窗口底部（保存提示等）
};

struct CtlItem
{
    HWND h = nullptr;   // 主控件
    HWND h2 = nullptr;  // 可选副控件（标签+下拉成对时：h=标签 h2=下拉）
    int pw = 0;         // 首选宽
    int ph = 0;         // 高
    int lw = 0;         // 成对项的标签宽（其余 0）
    int flags = 0;
};

std::vector<CtlItem> g_layout;
HBRUSH g_hBgBrush = nullptr;
int g_setMinW = 0, g_setMinH = 0; // WM_GETMINMAXINFO 下限（DPI 缩放后）
int g_setDpi = 96;
int g_scrollY = 0; // 设置页纵向滚动偏移（内容超出客户区时）

int MeasureTextW(HWND owner, HFONT f, const wchar_t* text)
{
    HDC dc = GetDC(owner);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
    return sz.cx;
}

int LayoutSettings()
{
    if (!g_hSettings) return 0;
    RECT rc{};
    GetClientRect(g_hSettings, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return 0;
    int pad = MulDiv(16, g_setDpi, 96), gap = MulDiv(10, g_setDpi, 96);

    // 底部锚定项先收集：内容流排完后放 max(内容尾, 客户底-高-pad)
    int bottomH = 0;
    for (const auto& it : g_layout)
        if (it.flags & IT_BOTTOM) bottomH += it.ph;

    int x = pad, y = pad, rowH = 0;
    int contentEnd = 0;
    for (const auto& it : g_layout)
    {
        if (it.flags & IT_BOTTOM) continue;
        int w = it.pw;
        if (it.flags & IT_SECTION) // 段标题：独行占满整行
        {
            w = cw - pad * 2;
            x = pad;
            y += rowH + (rowH ? gap * 2 : 0);
            rowH = 0;
        }
        else if (x > pad && x + w > cw - pad)
        {
            x = pad; // 放不下：换行
            y += rowH + gap;
            rowH = 0;
        }
        if (w > cw - pad * 2) w = cw - pad * 2;
        if (it.h2)
        {
            // 约定：h=标签（实测宽 lw），h2=控件（填满剩余）
            MoveWindow(it.h, x, y - g_scrollY, it.lw, it.ph, TRUE);
            MoveWindow(it.h2, x + it.lw + gap, y - g_scrollY, w - it.lw - gap, it.ph, TRUE);
        }
        else
        {
            MoveWindow(it.h, x, y - g_scrollY, w, it.ph, TRUE);
        }
        rowH = (rowH > it.ph) ? rowH : it.ph;
        x += w + gap;
        contentEnd = (contentEnd > y + rowH) ? contentEnd : y + rowH;
    }
    // 锚底项：不早于内容尾+间距，不晚于客户底-高-pad（内容短时贴底不滚动）
    int flowEnd = contentEnd + gap;
    int anchor = g_scrollY + ch - bottomH - pad;
    int hintY = (flowEnd > anchor) ? flowEnd : anchor;
    int by = hintY;
    for (const auto& it : g_layout)
    {
        if (!(it.flags & IT_BOTTOM)) continue;
        MoveWindow(it.h, pad, by - g_scrollY, cw - pad * 2, it.ph, TRUE);
        by += it.ph;
    }
    if (hintY > anchor) return by + pad; // 内容超出：虚拟内容总高
    return ch; // 内容贴底：恰好占满客户区，不触发滚动
}

// 滚动条开关+范围同步：内容超出客户区才显示 WS_VSCROLL（FRAMECHANGED 会
// 再触发一次 WM_SIZE→LayoutSettings，样式已一致，收敛）
void UpdateSettingsScrollbar(int contentH)
{
    if (!g_hSettings) return;
    RECT rc{};
    GetClientRect(g_hSettings, &rc);
    int clientH = rc.bottom;
    bool needScroll = contentH > clientH;
    LONG_PTR style = GetWindowLongPtrW(g_hSettings, GWL_STYLE);
    bool hasScroll = (style & WS_VSCROLL) != 0;
    if (needScroll != hasScroll)
    {
        SetWindowLongPtrW(g_hSettings, GWL_STYLE,
                          needScroll ? (style | WS_VSCROLL) : (style & ~WS_VSCROLL));
        SetWindowPos(g_hSettings, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    if (!needScroll)
    {
        if (g_scrollY != 0) { g_scrollY = 0; LayoutSettings(); }
        return;
    }
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = contentH;
    si.nPage = clientH > 0 ? (UINT)clientH : 1;
    int maxPos = contentH - clientH;
    if (g_scrollY > maxPos) g_scrollY = maxPos;
    if (g_scrollY < 0) g_scrollY = 0;
    si.nPos = g_scrollY;
    SetScrollInfo(g_hSettings, SB_VERT, &si, TRUE);
}

} // namespace

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
        case IDC_CMB_TOGGLEMODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) SaveFromControls();
            return 0;
        case IDC_BTN_USERDICT:
            CreateUserDictWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            return 0;
        case IDC_BTN_HOTPHRASES:
            ShowHotPhraseWindow((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), hwnd);
            return 0;
        }
        break;

    case WM_SIZE:
        UpdateSettingsScrollbar(LayoutSettings()); // 缩放→重排→滚动范围同步
        return 0;
    case WM_VSCROLL:
    {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int line = MulDiv(30, g_setDpi, 96);
        int pos = (int)si.nPos;
        switch (LOWORD(wParam))
        {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = (int)si.nTrackPos; break;
        default: return 0;
        }
        int maxPos = (int)si.nMax - (int)si.nPage;
        if (maxPos < 0) maxPos = 0;
        if (pos < 0) pos = 0;
        if (pos > maxPos) pos = maxPos;
        if (pos != g_scrollY)
        {
            g_scrollY = pos;
            UpdateSettingsScrollbar(LayoutSettings());
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        int delta = (short)HIWORD(wParam); // 正=前滚(向上)
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = g_scrollY - (delta / 40) * MulDiv(30, g_setDpi, 96); // 120→3 行
        int maxPos = (int)si.nMax - (int)si.nPage;
        if (maxPos < 0) maxPos = 0;
        if (pos < 0) pos = 0;
        if (pos > maxPos) pos = maxPos;
        if (pos != g_scrollY)
        {
            g_scrollY = pos;
            UpdateSettingsScrollbar(LayoutSettings());
        }
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        // 宽度下限=最窄可读；高度任意（超出靠纵向滚动）；上限防失控
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = g_setMinW;
        mmi->ptMinTrackSize.y = g_setMinH;
        mmi->ptMaxTrackSize.x = MulDiv(1600, g_setDpi, 96);
        mmi->ptMaxTrackSize.y = MulDiv(2200, g_setDpi, 96);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        // 浅底配色：静态文本/复选框文字统一灰黑，背景与窗口底色一致
        SetBkColor((HDC)wParam, RGB(248, 250, 252));
        SetTextColor((HDC)wParam, RGB(64, 68, 78));
        return (LRESULT)g_hBgBrush;
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
    if (!g_hBgBrush)
        g_hBgBrush = CreateSolidBrush(RGB(248, 250, 252));
    wc.hbrBackground = g_hBgBrush; // 浅底配色；缩放时正确擦除背景
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);

    // 随 DPI 简单缩放（96 基准）；WS_THICKFRAME 可拖拽缩放，WM_SIZE 流式重排
    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int scale = (int)dpi;
    g_setDpi = (int)dpi;
    int rowH = MulDiv(30, scale, 96);
    int comboListUnits = 8; // 下拉弹开高度=行高倍数（条目多）

    // 关键：创建子控件期间 Edit 会触发 EN_CHANGE、复选框初始为未勾选，若此时
    // 回写会把所有设置污染为 0。整个控件创建期间屏蔽保存，直到 LoadToControls
    // 正确加载后才放开（LoadToControls 结束时会把 g_updating 复位为 false）。
    g_updating = true;
    g_layout.clear();

    g_hSettings = CreateWindowExW(0, kSettingsWndClass, L"灵犀输入法设置",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                  WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  MulDiv(680, scale, 96), MulDiv(560, scale, 96),
                                  NULL, NULL, hInst, NULL);

    g_hFont = CreateFontW(-MulDiv(12, scale, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    // ---- 注册式创建：所有控件进 g_layout，位置由 LayoutSettings() 统一排布 ----
    auto addSection = [&](const wchar_t* text) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 0, 0, 10, rowH, g_hSettings, NULL, hInst, NULL);
        g_layout.push_back({ h, nullptr, MulDiv(10, scale, 96), rowH, 0, IT_SECTION });
        return h;
    };
    auto addCheck = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 10, rowH, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        // 首选宽=文字实测+勾选框留白；放不下自动换行/截断
        int tw = MeasureTextW(g_hSettings, g_hFont, text) + MulDiv(30, scale, 96);
        g_layout.push_back({ h, nullptr, tw, rowH, 0, IT_WRAP });
        return h;
    };
    // 标签+下拉成对：一个布局单元，标签左（实测宽+垂直居中）、控件填余宽
    auto addComboPair = [&](const wchar_t* label, int id, int comboW, int listUnits,
                            void (*fill)(HWND)) -> HWND {
        HWND hLab = CreateWindowExW(0, L"STATIC", label,
                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                    0, 0, 10, rowH, g_hSettings, NULL, hInst, NULL);
        HWND hCmb = CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                    0, 0, 10, rowH * listUnits, g_hSettings,
                                    (HMENU)(INT_PTR)id, hInst, NULL);
        if (fill) fill(hCmb);
        int labW = MeasureTextW(g_hSettings, g_hFont, label) + MulDiv(8, scale, 96);
        g_layout.push_back({ hLab, hCmb, labW + MulDiv(10, scale, 96) + comboW,
                             rowH, labW, IT_WRAP });
        return hCmb;
    };
    auto addButton = [&](const wchar_t* text, int id, int w) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 0, 0, 10, rowH, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        g_layout.push_back({ h, nullptr, w, rowH, 0, IT_WRAP });
        return h;
    };

    // ---- 输入 ----
    addSection(L"输入");
    g_hChkPinyin = addCheck(L"拼音组合模式（字母上屏汉字，空格/数字选择；关闭则字母直通+英文补全）", IDC_CHK_PINYIN);
    g_hChkEn = addCheck(L"英文逐键补全（词中自动补全英文单词）", IDC_CHK_EN);
    g_hChkFuzzy = addCheck(L"模糊音（z/zh、c/ch、s/sh、n/l、前后鼻音互通，精确优先）", IDC_CHK_FUZZY);
    g_hChkCnPunct = addCheck(L"中文标点（. , / ; 等自动转全角，Shift 出 ？：》《 等）", IDC_CHK_CNPUNCT);
    g_hChkVMode = addCheck(L"v 模式（中文下首字母 v 直通英文，如 vhello 出 hello）", IDC_CHK_VMODE);
    g_hChkShuangpin = addCheck(L"小鹤双拼（每音节两键：声母+韵母，零声母按韵母首字母补齐）", IDC_CHK_SHUANGPIN);
    g_hChkTraditional = addCheck(L"繁体输出（候选简体字转为繁体）", IDC_CHK_TRADITIONAL);
    // B5 草案②+自定义扩展：中英切换键五态下拉
    auto fillToggle = [](HWND h) {
        for (const wchar_t* s : { L"关闭", L"Shift", L"Shift 或 Ctrl", L"CapsLock", L"Ctrl" })
            ComboBox_AddString(h, s);
    };
    g_hCmbToggleMode = addComboPair(L"中英切换键（单击切换，显示中/英提示）",
                                    IDC_CMB_TOGGLEMODE, MulDiv(150, scale, 96), comboListUnits, fillToggle);
    g_hChkSymbolHotkey = addCheck(L"符号面板热键 Ctrl+Shift+B（全局独占，默认关，面板可从托盘打开）", IDC_CHK_SYMBOLHOTKEY);

    // ---- 外观 ----
    addSection(L"外观");
    auto fillDark = [](HWND h) {
        for (const wchar_t* s : { L"跟随应用", L"浅色", L"深色" })
            ComboBox_AddString(h, s);
    };
    g_hCmbDark = addComboPair(L"候选窗明暗", IDC_CMB_DARK, MulDiv(150, scale, 96), comboListUnits, fillDark);
    auto fillFont = [](HWND h) {
        for (uint32_t z : {12u, 14u, 16u, 18u, 20u, 24u, 28u, 32u})
        {
            wchar_t b[8];
            _snwprintf_s(b, _TRUNCATE, L"%u", z);
            ComboBox_AddString(h, b);
        }
    };
    g_hCmbFontSize = addComboPair(L"候选字号", IDC_CMB_FONTSIZE, MulDiv(100, scale, 96), comboListUnits, fillFont);
    auto fillSkin = [](HWND h) {
        for (const wchar_t* s : { L"蓝色", L"青色", L"紫色", L"绿色", L"橙色" })
            ComboBox_AddString(h, s);
    };
    g_hCmbSkin = addComboPair(L"界面配色", IDC_CMB_SKIN, MulDiv(100, scale, 96), comboListUnits, fillSkin);

    // ---- 词典与工具 ----
    addSection(L"词典与工具");
    addButton(L"用户词典与自定义词条...", IDC_BTN_USERDICT, MulDiv(210, scale, 96));
    addButton(L"快捷短语...", IDC_BTN_HOTPHRASES, MulDiv(140, scale, 96));

    // ---- 智能特性 ----
    addSection(L"智能特性");
    g_hChkLlmRerank = addCheck(L"LLM 上下文智能重排（实验：停顿打完一字后按上文重排候选，增加 CPU）", IDC_CHK_LLMRERANK);
    HWND hLabCount = CreateWindowExW(0, L"STATIC", L"英文补全候选数（1-9）",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                     0, 0, 10, rowH, g_hSettings, NULL, hInst, NULL);
    g_hEdtCount = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"9",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                  0, 0, 10, rowH, g_hSettings,
                                  (HMENU)(INT_PTR)IDC_EDT_COUNT, hInst, NULL);
    {
        int labW = MeasureTextW(g_hSettings, g_hFont, L"英文补全候选数（1-9）") + MulDiv(8, scale, 96);
        g_layout.push_back({ hLabCount, g_hEdtCount, labW + MulDiv(10, scale, 96) + MulDiv(64, scale, 96),
                             rowH, labW, IT_WRAP });
    }

    // ---- 底部提示（锚底，缩放时始终可见）----
    HWND hHint = CreateWindowExW(0, L"STATIC",
        L"修改即时生效并自动保存。重建输入法后需重启应用才会加载新版本。",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, rowH, g_hSettings, NULL, hInst, NULL);
    g_layout.push_back({ hHint, nullptr, MulDiv(10, scale, 96), rowH, 0, IT_BOTTOM });

    EnumChildWindows(g_hSettings, [](HWND h, LPARAM lp) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)g_hFont);

    // 宽度下限=最窄可读；高度下限宽松（内容超出靠纵向滚动）
    g_setMinW = MulDiv(460, scale, 96);
    g_setMinH = MulDiv(380, scale, 96);

    LoadToControls();
    UpdateSettingsScrollbar(LayoutSettings());
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
