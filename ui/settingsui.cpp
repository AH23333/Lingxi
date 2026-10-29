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
// GDI+：抗锯齿圆角/圆绘制（GDI RoundRect/Ellipse 无 AA，药丸/卡片边缘锯齿）
#define NOMINMAX
#include <gdiplus.h>
#include <algorithm>
#pragma comment(lib, "gdiplus.lib")

#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kMainWndClass[] = L"LingxiUIMainWnd";
constexpr wchar_t kSettingsWndClass[] = L"LingxiSettingsWnd";
constexpr wchar_t kUserDictWndClass[] = L"LingxiUserDictWnd";
constexpr UINT WM_APP_TRAY = WM_APP + 10; // 托盘回调消息

std::function<void()> g_reloadUserDict; // 用户词典变更后重新加载引擎
std::function<void()> g_showSymbols;   // 托盘菜单：打开符号面板
std::function<void()> g_settingsChanged; // 设置保存后（同步热键等）

// ---------------- GDI+ 抗锯齿绘制助手 ----------------
// GDI RoundRect/Ellipse 无抗锯齿，真机肉眼可见锯齿；GDI+ GraphicsPath+
// SmoothingModeAntiAlias 根治。令牌进程生命周期持有（UI 进程退出由系统回收）。

ULONG_PTR g_gdipToken = 0;

void EnsureGdip()
{
    if (g_gdipToken) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdipToken, &in, NULL);
}

Gdiplus::Color GdipColor(COLORREF c)
{
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 填充圆角矩形（可选描边）。radius=高度一半即药丸形。
void GdipRoundRect(HDC hdc, RECT rc, COLORREF fill, int radius,
                   COLORREF border = 0, int borderW = 0)
{
    EnsureGdip();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    int d = radius * 2;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    d = (std::min)(d, (std::min)(w, h));
    Gdiplus::Rect r(rc.left, rc.top, w, h);
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + w - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + w - d, r.Y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::SolidBrush br(GdipColor(fill));
    g.FillPath(&br, &path);
    if (borderW > 0)
    {
        Gdiplus::Pen pen(GdipColor(border), (Gdiplus::REAL)borderW);
        pen.SetAlignment(Gdiplus::PenAlignmentInset); // 描边不溢出控件矩形
        g.DrawPath(&pen, &path);
    }
}

void GdipFillEllipse(HDC hdc, RECT rc, COLORREF fill)
{
    EnsureGdip();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(GdipColor(fill));
    g.FillEllipse(&br, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

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
constexpr int IDC_CHK_LLMMULTIPATH = 119; // LLM 多路采样（R3 §3，默认关）
constexpr int IDC_BTN_HOTPHRASES = 118; // 快捷短语管理入口

HWND g_hMain = nullptr;    // 隐藏消息窗口（托盘宿主）
HWND g_hSettings = nullptr; // 设置窗口（关闭=隐藏，常驻）
HWND g_hChkPinyin = nullptr, g_hChkEn = nullptr, g_hEdtCount = nullptr,
     g_hCmbDark = nullptr, g_hCmbFontSize = nullptr, g_hCmbSkin = nullptr,
     g_hCmbToggleMode = nullptr,
     g_hChkCnPunct = nullptr, g_hChkFuzzy = nullptr,
     g_hChkVMode = nullptr,
     g_hChkShuangpin = nullptr, g_hChkSymbolHotkey = nullptr, g_hChkTraditional = nullptr,
      g_hChkLlmRerank = nullptr, g_hChkLlmMultiPath = nullptr;
HFONT g_hFont = nullptr;        // 12px：通用控件（用户词典窗沿用）
HFONT g_hFontLabel = nullptr;   // 设置行标题 14px
HFONT g_hFontDesc = nullptr;    // 设置行描述 12px（次级色）
HFONT g_hFontTitle = nullptr;   // 设置页标题 20px 半粗
HFONT g_hFontNav = nullptr;     // 设置导航标题 15px 半粗
HFONT g_hFontCardHdr = nullptr; // 设置卡片标题 14px 半粗
HBRUSH g_hBgBrush = nullptr;    // WinUI 中性窗体底
HBRUSH g_hCardBrush = nullptr;  // WinUI 卡片白
bool g_updating = false; // 程序性改控件时不回写设置
bool g_controlsLoaded = false; // 控件尚未从设置加载同步——禁止回写（防刷零事故）
// BS_AUTOCHECKBOX|BS_OWNERDRAW 位或后=0xB=纯 BS_OWNERDRAW：自绘按钮不维护勾选
// 态，BM_GETCHECK 恒返 0——历史两次"整页刷零"事故与本次设置全零的真凶。
// 开关状态改由此映射外置维护，绘制/读写/点击翻转全走它。
std::unordered_map<HWND, bool> g_toggleOn;
void SetToggle(HWND h, bool on)
{
    if (!h) return;
    g_toggleOn[h] = on;
    InvalidateRect(h, NULL, TRUE); // owner-draw 无自动重绘：翻转必须手动失效
}
bool ToggleOn(HWND h)
{
    auto it = g_toggleOn.find(h);
    return it != g_toggleOn.end() && it->second;
}

HICON g_hIcon = nullptr;
UINT g_taskbarCreatedMsg = 0;
bool g_trayAdded = false;

// ---------------- 设置读写 ----------------

void LoadToControls()
{
    Settings s = LoadSettings();
    g_updating = true;
    SetToggle(g_hChkPinyin, s.pinyinEnabled);
    SetToggle(g_hChkEn, s.enCompletion);
    SetToggle(g_hChkCnPunct, s.cnPunct);
    SetToggle(g_hChkFuzzy, s.fuzzyPinyin);
    ComboBox_SetCurSel(g_hCmbToggleMode, (int)(s.toggleMode <= 4 ? s.toggleMode : 1));
    SetToggle(g_hChkVMode, s.vMode);
    SetToggle(g_hChkShuangpin, s.shuangpin);
    SetToggle(g_hChkSymbolHotkey, s.symbolHotkey);
    SetToggle(g_hChkTraditional, s.traditionalOutput);
    SetToggle(g_hChkLlmRerank, s.llmRerank);
    SetToggle(g_hChkLlmMultiPath, s.llmMultiPath);
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
    g_controlsLoaded = true;
    g_updating = false;
}

void SaveFromControls()
{
    // 双保险（2026-10-29 复盘：真实设置两次被"整页回写未同步控件"刷零）：
    // ① 控件从未 LoadToControls 过 → 拒写；② 改用 diff 落盘，未变字段不触碰。
    if (g_updating || !g_controlsLoaded) return;
    Settings s = LoadSettings(); // 保留未在此窗口编辑的字段
    s.pinyinEnabled = ToggleOn(g_hChkPinyin);
    s.enCompletion = ToggleOn(g_hChkEn);
    s.cnPunct = ToggleOn(g_hChkCnPunct);
    s.fuzzyPinyin = ToggleOn(g_hChkFuzzy);
    int tsel = ComboBox_GetCurSel(g_hCmbToggleMode);
        if (tsel >= 0 && tsel <= 4) s.toggleMode = (uint32_t)tsel;
    s.vMode = ToggleOn(g_hChkVMode);
    s.shuangpin = ToggleOn(g_hChkShuangpin);
    s.symbolHotkey = ToggleOn(g_hChkSymbolHotkey);
    s.traditionalOutput = ToggleOn(g_hChkTraditional);
    s.llmRerank = ToggleOn(g_hChkLlmRerank);
    s.llmMultiPath = ToggleOn(g_hChkLlmMultiPath);
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
    SaveSettingsDiff(s);
    if (g_settingsChanged) g_settingsChanged();
    if (g_hSettings) InvalidateRect(g_hSettings, NULL, TRUE); // 换肤等即时重绘自绘控件
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
    if (sel < 0)
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"请先在列表中选择词条");
        return;
    }
    // 按内容匹配删除：引擎自学习会随时重写词典文件（学习即落盘），用户浏览
    // 期间列表索引可能已与文件漂移——按索引删会删错条目或静默落空（此前
    // "能加不能删"的实感根因）。列表行格式 "word    key    count"（4 空格分隔）。
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
    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    bool removed = false;
    if (!word.empty() && !key.empty())
    {
        for (size_t i = 0; i < entries.size(); i++)
        {
            if (entries[i].word == word && entries[i].key == key)
            {
                entries.erase(entries.begin() + i);
                removed = true;
                break;
            }
        }
    }
    if (!removed)
    {
        // 内容匹配失败（旧格式行/并发已删）：回退索引路径
        if (sel < (int)entries.size()) { entries.erase(entries.begin() + sel); removed = true; }
    }
    if (!removed)
    {
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC, L"该词条已不存在（可能刚被学习进程更新），列表已刷新");
        RefreshUdList();
        return;
    }
    PersistUdEntries(entries);
    RefreshUdList();
    if (removed)
    {
        int n = (int)SendMessageW(g_hUdList, LB_GETCOUNT, 0, 0);
        if (n > 0) SendMessageW(g_hUdList, LB_SETCURSEL, sel < n ? sel : n - 1, 0);
        SetDlgItemTextW(g_hUserDict, IDC_UD_STATIC,
            L"已删除并重新加载（基础词典里的常见词仍会出现在候选中）");
    }
    LX_LOG(L"ui", L"用户词典删除: %ls (key=%ls)", word.c_str(), key.c_str());
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
    // 同 UdDeleteSelected：按内容匹配（引擎并发重写词典文件，索引会漂移）
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
    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);
    bool found = false;
    if (!word.empty() && !key.empty())
    {
        for (size_t i = 0; i < entries.size(); i++)
        {
            if (entries[i].word == word && entries[i].key == key)
            {
                entries[i].count += delta;
                if (entries[i].count < 1) entries[i].count = 1;
                found = true;
                break;
            }
        }
    }
    if (!found)
    {
        if (sel >= (int)entries.size()) return;
        entries[sel].count += delta;
        if (entries[sel].count < 1) entries[sel].count = 1;
    }
    if (!WriteFileBytes(UserDictPathW(), SerializeUserDict(entries)))
    {
        MessageBoxW(g_hUserDict, L"用户词典写入失败（文件被占用或无权限），修改未保存。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_reloadUserDict) g_reloadUserDict();
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

// Win11 设置风格三级模型：页面（左导航项）/ 卡片（白卡+标题）/ 行（标题+
// 描述+右侧控件）。控件一次创建、按页显隐；LayoutSettings() 只排当前页，
// PaintSettings() 绘制导航/卡面/分隔线/底部提示。
namespace {

// 右侧控件形态
enum CtlKind
{
    CK_TOGGLE, // Win11 药丸开关（BS_OWNERDRAW 自绘，状态外置 g_toggleOn）
    CK_COMBO,  // CBS_DROPDOWNLIST 下拉
    CK_EDIT,   // 数字输入
    CK_BUTTON, // 强调色按钮（词典/短语入口）
    CK_NONE,   // 纯文本行（无控件）
};

struct SetRow
{
    HWND lab = nullptr;   // 行标题
    HWND desc = nullptr;  // 行描述（可空）
    HWND ctl = nullptr;   // 右侧控件（可空）
    CtlKind kind = CK_NONE;
    int ctlW = 0;         // 控件宽（96dpi 基准）
};

struct SetCard
{
    const wchar_t* header = nullptr; // 卡片标题（PaintSettings 绘制，可空）
    std::vector<SetRow> rows;
    RECT rc{};             // 排布后的卡面（客户区坐标，含滚动偏移）
    std::vector<int> divY; // 行分隔线 y（客户区坐标）
};

struct SetPage
{
    const wchar_t* nav = nullptr; // 导航项文本 = 页标题
    std::vector<SetCard> cards;
};

std::vector<SetPage> g_pages;
std::vector<HWND> g_descWnds;    // 描述文本框（CTLCOLOR 用次级色）
int g_curPage = 0;               // 当前页索引
int g_navHover = -1;             // 导航 hover 项（-1 无）
bool g_trackingMouse = false;    // TrackMouseEvent 已挂
int g_titleY = 0;                // 页标题 y（PaintSettings 用）
std::vector<HWND> g_accentBtns;  // 强调色按钮（WM_DRAWITEM 分发用）
int g_setMinW = 0, g_setMinH = 0; // WM_GETMINMAXINFO 下限（DPI 缩放后）
int g_setDpi = 96;
int g_scrollY = 0; // 设置页纵向滚动偏移（内容超出客户区时）
constexpr int kNavW = 216; // 96dpi 基准导航宽

int Scaled(int v) { return MulDiv(v, g_setDpi, 96); }

// 行高：带描述 60、纯标题 44（96dpi 基准）
int RowHeight(const SetRow& r) { return Scaled(r.desc ? 60 : 44); }

// 当前页行控件按 g_curPage 显隐（页面切换调用）
void ApplyPageVisibility()
{
    for (size_t p = 0; p < g_pages.size(); p++)
        for (auto& card : g_pages[p].cards)
            for (auto& row : card.rows)
            {
                int show = p == (size_t)g_curPage ? SW_SHOW : SW_HIDE;
                if (row.lab) ShowWindow(row.lab, show);
                if (row.desc) ShowWindow(row.desc, show);
                if (row.ctl) ShowWindow(row.ctl, show);
            }
}

// 排布当前页：页标题 → 逐卡（标题区/行/分隔线）——标签/描述左侧，控件右
// 缘对齐垂直居中。返回虚拟内容总高（滚动范围用）。
int LayoutSettings()
{
    if (!g_hSettings || g_pages.empty()) return 0;
    RECT rc{};
    GetClientRect(g_hSettings, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return 0;
    int navW = Scaled(kNavW);
    int pad = Scaled(24);     // 内容区外边距
    int cardPad = Scaled(16); // 卡片内边距
    int gap = Scaled(12);     // 卡片间距
    int x0 = navW + pad;
    int cardW = cw - x0 - pad;
    if (cardW < Scaled(240)) cardW = Scaled(240);

    g_titleY = Scaled(20) - g_scrollY;
    int y = g_titleY + Scaled(40) + Scaled(8);
    for (auto& card : g_pages[g_curPage].cards)
    {
        int top = y;
        card.divY.clear();
        if (card.header) // 卡片标题区 + 标题下分隔线
        {
            y += Scaled(38);
            card.divY.push_back(y);
        }
        else
        {
            y += Scaled(12);
        }
        bool first = true;
        for (auto& row : card.rows)
        {
            if (!first && row.lab) card.divY.push_back(y); // 行间分隔线
            int rh = RowHeight(row);
            int textW = cardW - cardPad * 2 - (row.ctl ? Scaled(row.ctlW) + Scaled(24) : 0);
            if (row.lab)
                MoveWindow(row.lab, x0 + cardPad, y + (row.desc ? Scaled(8) : Scaled(11)),
                           textW, Scaled(22), TRUE);
            if (row.desc)
                MoveWindow(row.desc, x0 + cardPad, y + Scaled(33), textW, Scaled(18), TRUE);
            if (row.ctl)
            {
                int ctlH = 0;
                switch (row.kind)
                {
                case CK_TOGGLE: ctlH = Scaled(24); break;
                case CK_COMBO: ctlH = Scaled(30); break;
                case CK_EDIT: ctlH = Scaled(30); break;
                case CK_BUTTON: ctlH = Scaled(32); break;
                default: break;
                }
                MoveWindow(row.ctl, x0 + cardW - cardPad - Scaled(row.ctlW),
                           y + (rh - ctlH) / 2, Scaled(row.ctlW), ctlH, TRUE);
            }
            y += rh;
            first = false;
        }
        y += Scaled(12); // 卡片底内边距
        card.rc = { x0, top, x0 + cardW, y };
        y += gap;
    }
    return y - gap + g_scrollY + Scaled(56); // 底部提示保护区
}

// Win11 观感统一绘制：中性底 + 导航（选中药丸+左缘强调条）+ 页标题 + 白卡
// 圆角描边 + 行分隔线 + 底部提示。子控件（标签/开关/下拉）由系统随后自绘。
void PaintSettings(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0 || g_pages.empty() || !g_hFontNav)
    {
        EndPaint(hwnd, &ps);
        return;
    }

    HBRUSH bg = CreateSolidBrush(kUiBg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    int navW = Scaled(kNavW);
    COLORREF accent = UiSkin(LoadSettings().skinIndex).accent;
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(hdc, g_hFontNav);

    // 导航标题
    SetTextColor(hdc, kUiText);
    RECT ntr{ Scaled(18), Scaled(14), navW, Scaled(44) };
    DrawTextW(hdc, L"灵犀输入法", -1, &ntr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 导航项：选中/热态浅灰药丸 + 选中左缘 3px 强调条
    SelectObject(hdc, g_hFontLabel);
    int itemH = Scaled(36), navTop = Scaled(56);
    for (int i = 0; i < (int)g_pages.size(); i++)
    {
        RECT item{ Scaled(10), navTop + i * itemH, navW - Scaled(10),
                   navTop + (i + 1) * itemH - Scaled(2) };
        bool sel = i == g_curPage;
        if (sel || i == g_navHover)
        {
            GdipRoundRect(hdc, item, sel ? RGB(229, 229, 229) : RGB(238, 238, 238), 8);
        }
        if (sel)
        {
            HBRUSH ib = CreateSolidBrush(accent);
            RECT bar{ item.left + Scaled(6), item.top + Scaled(8),
                      item.left + Scaled(9), item.bottom - Scaled(8) };
            FillRect(hdc, &bar, ib);
            DeleteObject(ib);
        }
        RECT ttr{ item.left + Scaled(18), item.top, item.right - Scaled(4), item.bottom };
        DrawTextW(hdc, g_pages[i].nav, -1, &ttr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 页标题
    int x0 = navW + Scaled(24);
    SelectObject(hdc, g_hFontTitle);
    RECT ptr{ x0, g_titleY, cw - Scaled(8), g_titleY + Scaled(38) };
    DrawTextW(hdc, g_pages[g_curPage].nav, -1, &ptr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 白卡（8px 圆角 + 1px 浅描边，GDI+ 抗锯齿）
    int rr = Scaled(16);
    for (auto& card : g_pages[g_curPage].cards)
        GdipRoundRect(hdc, card.rc, kUiCard, rr, kUiCardBorder, 1);

    // 卡片标题 + 行分隔线
    HPEN lpen = CreatePen(PS_SOLID, 1, RGB(238, 238, 238));
    HGDIOBJ lp = SelectObject(hdc, lpen);
    for (auto& card : g_pages[g_curPage].cards)
    {
        if (card.header)
        {
            SelectObject(hdc, g_hFontCardHdr);
            RECT htr{ card.rc.left + Scaled(16), card.rc.top + Scaled(8),
                      card.rc.right - Scaled(8), card.rc.top + Scaled(38) };
            DrawTextW(hdc, card.header, -1, &htr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        for (int dy : card.divY)
        {
            MoveToEx(hdc, card.rc.left + Scaled(16), dy, NULL);
            LineTo(hdc, card.rc.right - Scaled(16), dy);
        }
    }
    SelectObject(hdc, lp);
    DeleteObject(lpen);

    // 底部提示（内容区左下，不随滚动）
    SelectObject(hdc, g_hFontDesc);
    SetTextColor(hdc, kUiText2);
    RECT ftr{ x0, ch - Scaled(34), cw - Scaled(16), ch - Scaled(10) };
    DrawTextW(hdc, L"修改即时生效并自动保存 · 重建输入法后需重启应用加载新版",
              -1, &ftr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
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
        if (g_scrollY != 0)
        {
            g_scrollY = 0;
            LayoutSettings();
            InvalidateRect(g_hSettings, NULL, TRUE); // 卡片随内容回移重绘
        }
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

// ---- Win11 风格 owner-draw：药丸开关 / 强调色按钮 ----

// Win11 开关（ToggleSwitch）：44×24 热区内绘 40×20 全圆角药丸——
// 开=皮肤强调色+白钮居右；关=浅灰底+深灰钮居左+1px 灰描边。
// 纯 BS_OWNERDRAW：状态读外置 g_toggleOn（勾选位会被 OWNERDRAW 归并吞掉）。
void DrawToggle(const DRAWITEMSTRUCT* dis)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    // 控件背景补白（BUTTON 默认 BTNFACE 擦除会露灰，须盖成卡片白）
    HBRUSH hCard = CreateSolidBrush(kUiCard);
    FillRect(hdc, &rc, hCard);
    DeleteObject(hCard);

    int pillW = MulDiv(40, g_setDpi, 96), pillH = MulDiv(20, g_setDpi, 96);
    int px = rc.left + ((rc.right - rc.left) - pillW) / 2;
    int py = rc.top + ((rc.bottom - rc.top) - pillH) / 2;
    RECT pr{ px, py, px + pillW, py + pillH };
    bool on = ToggleOn(dis->hwndItem); // 外置状态（BS_OWNERDRAW 无勾选态）
    COLORREF accent = UiSkin(LoadSettings().skinIndex).accent;

    // GDI+ 抗锯齿药丸（GDI RoundRect 无 AA，真机边缘锯齿）
    if (on)
        GdipRoundRect(hdc, pr, accent, pillH / 2);
    else // 关：1px 灰描边（浅灰底在白卡上要有可见边界）
        GdipRoundRect(hdc, pr, RGB(234, 234, 234), pillH / 2, kUiControlEdge, 1);

    // 滑钮：12px 圆（开=白居右；关=深灰居左），GDI+ 抗锯齿
    int knob = MulDiv(12, g_setDpi, 96);
    int kx = on ? pr.right - knob - MulDiv(4, g_setDpi, 96)
                : pr.left + MulDiv(4, g_setDpi, 96);
    int ky = pr.top + (pillH - knob) / 2;
    GdipFillEllipse(hdc, RECT{ kx, ky, kx + knob, ky + knob },
                    on ? RGB(255, 255, 255) : RGB(90, 90, 90));
}

// 强调色按钮：皮肤色圆角底 + 白字（按下加深 25%）
void DrawAccentBtn(const DRAWITEMSTRUCT* dis)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF accent = UiSkin(LoadSettings().skinIndex).accent;
    if (pressed)
        accent = RGB(GetRValue(accent) * 3 / 4, GetGValue(accent) * 3 / 4,
                     GetBValue(accent) * 3 / 4);
    GdipRoundRect(hdc, rc, accent, 4);

    wchar_t txt[128] = {};
    GetWindowTextW(dis->hwndItem, txt, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(hdc, g_hFontLabel);
    DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

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
        case IDC_CHK_LLMMULTIPATH:
            // owner-draw 无自动勾选：先翻外置状态再回写（翻转即失效重绘药丸）
            if (HIWORD(wParam) == BN_CLICKED)
            {
                SetToggle((HWND)lParam, !ToggleOn((HWND)lParam));
                SaveFromControls();
            }
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
    case WM_PAINT:
        PaintSettings(hwnd); // 导航/卡片/标题/分隔线/底部提示统一绘制
        return 0;
    case WM_ERASEBKGND:
    {
        // 中性底一次铺满即可：导航/卡片/文本全部在 WM_PAINT 里绘制
        HDC hdc = (HDC)wParam;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBgBrush);
        return 1;
    }
    case WM_LBUTTONDOWN:
    {
        // 导航项命中→换页：回顶重排全刷（其余区域点击走默认，不夺编辑焦点）
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int navTop = Scaled(56), itemH = Scaled(36);
        if (x >= Scaled(10) && x < Scaled(kNavW) && y >= navTop)
        {
            int idx = (y - navTop) / itemH;
            if (idx >= 0 && idx < (int)g_pages.size() && idx != g_curPage)
            {
                g_curPage = idx;
                g_scrollY = 0; // 换页回顶
                ApplyPageVisibility();
                UpdateSettingsScrollbar(LayoutSettings());
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        break;
    }
    case WM_MOUSEMOVE:
    {
        // 导航 hover 药丸；离开导航区即时消隐（TrackMouseEvent 挂 WM_MOUSELEAVE）
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int navTop = Scaled(56), itemH = Scaled(36);
        int hover = -1;
        if (x >= Scaled(10) && x < Scaled(kNavW) && y >= navTop)
        {
            int idx = (y - navTop) / itemH;
            if (idx >= 0 && idx < (int)g_pages.size()) hover = idx;
        }
        if (!g_trackingMouse)
        {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            if (TrackMouseEvent(&tme)) g_trackingMouse = true;
        }
        if (hover != g_navHover)
        {
            g_navHover = hover;
            RECT navRc{ 0, 0, Scaled(kNavW), Scaled(56) + (int)g_pages.size() * itemH };
            InvalidateRect(hwnd, &navRc, FALSE); // 只刷导航列
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (g_navHover != -1)
        {
            g_navHover = -1;
            g_trackingMouse = false;
            RECT navRc{ 0, 0, Scaled(kNavW), Scaled(56) + (int)g_pages.size() * Scaled(36) };
            InvalidateRect(hwnd, &navRc, FALSE);
        }
        return 0;
    case WM_DRAWITEM:
    {
        auto* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlType == ODT_BUTTON)
        {
            if (g_toggleOn.find(dis->hwndItem) != g_toggleOn.end())
            {
                DrawToggle(dis); // 药丸开关（状态外置 g_toggleOn）
                return TRUE;
            }
            for (HWND h : g_accentBtns)
                if (h == dis->hwndItem) { DrawAccentBtn(dis); return TRUE; }
        }
        break;
    }
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
            InvalidateRect(hwnd, NULL, TRUE); // 卡片/控件整体随滚动重绘
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
            InvalidateRect(hwnd, NULL, TRUE);
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
        // 卡片上的静态文本：白底主文本；行描述（g_descWnds）：白底次级文本
        for (HWND h : g_descWnds)
        {
            if ((HWND)lParam == h)
            {
                SetBkColor((HDC)wParam, kUiCard);
                SetTextColor((HDC)wParam, kUiText2);
                return (LRESULT)g_hCardBrush;
            }
        }
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hCardBrush;
    case WM_CTLCOLORBTN:
        // owner-draw 勾选框背景擦除（默认 BTNFACE 会露灰，须给卡片白刷）
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hCardBrush;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hCardBrush;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); // 关闭=隐藏，保留控件状态
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateSettingsWindow(HINSTANCE hInst)
{
    if (g_hSettings) { LX_LOG(L"ui", L"设置窗复用 isIcon=%d", IsWindow(g_hSettings)); ShowWindow(g_hSettings, SW_SHOW); SetForegroundWindow(g_hSettings); return; } // FORWARD-DIAG

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!g_hBgBrush)
        g_hBgBrush = CreateSolidBrush(kUiBg);
    if (!g_hCardBrush)
        g_hCardBrush = CreateSolidBrush(kUiCard);
    wc.hbrBackground = g_hBgBrush; // WinUI 中性窗体底；缩放时正确擦除背景
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);

    // 随 DPI 简单缩放（96 基准）；WS_THICKFRAME 可拖拽缩放，WM_SIZE 重排
    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int scale = (int)dpi;
    g_setDpi = (int)dpi;
    int comboListUnits = 8; // 下拉弹开高度=行高倍数（条目多）

    // 关键：创建子控件期间 Edit 会触发 EN_CHANGE、复选框初始为未勾选，若此时
    // 回写会把所有设置污染为 0。整个控件创建期间屏蔽保存，直到 LoadToControls
    // 正确加载后才放开（LoadToControls 结束时会把 g_updating 复位为 false）。
    g_updating = true;
    g_pages.clear();
    g_descWnds.clear();
    g_accentBtns.clear();
    g_curPage = 0;
    g_navHover = -1;

    g_hSettings = CreateWindowExW(WS_EX_COMPOSITED, kSettingsWndClass, L"灵犀输入法设置",
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                   WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   MulDiv(900, scale, 96), MulDiv(680, scale, 96),
                                   NULL, NULL, hInst, NULL);

    auto mkFont = [scale](int px, int weight) {
        return CreateFontW(-MulDiv(px, scale, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };
    g_hFont = mkFont(12, FW_NORMAL);          // 通用控件（用户词典窗沿用）
    g_hFontLabel = mkFont(14, FW_NORMAL);     // 行标题
    g_hFontDesc = mkFont(12, FW_NORMAL);      // 行描述
    g_hFontTitle = mkFont(20, FW_SEMIBOLD);   // 页标题
    g_hFontNav = mkFont(15, FW_SEMIBOLD);     // 导航标题
    g_hFontCardHdr = mkFont(14, FW_SEMIBOLD); // 卡片标题

    // ---- Win11 设置结构：页→卡→行。控件先创建（不可见），mkRow 挂进模型，
    // ApplyPageVisibility 按当前页显隐，LayoutSettings 统一排布 ----
    auto addPage = [&](const wchar_t* nav) {
        g_pages.push_back(SetPage{ nav });
    };
    auto addCard = [&](const wchar_t* header) {
        g_pages.back().cards.push_back(SetCard{ header });
    };
    auto mkRow = [&](const wchar_t* title, const wchar_t* desc, HWND ctl, CtlKind kind, int ctlW) {
        SetRow row;
        row.lab = CreateWindowExW(0, L"STATIC", title,
                                  WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                                  0, 0, 10, 22, g_hSettings, NULL, hInst, NULL);
        if (desc)
        {
            row.desc = CreateWindowExW(0, L"STATIC", desc,
                                       WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                                       0, 0, 10, 18, g_hSettings, NULL, hInst, NULL);
            g_descWnds.push_back(row.desc);
        }
        row.ctl = ctl;
        row.kind = kind;
        row.ctlW = ctlW;
        g_pages.back().cards.back().rows.push_back(row);
    };
    auto mkToggle = [&](int id) -> HWND {
        // 不能写 BS_AUTOCHECKBOX|BS_OWNERDRAW：低位样式位或后=0xB 归并为纯
        // BS_OWNERDRAW，勾选态静默丢失（BM_GETCHECK 恒 0 → 历史刷零真凶）。
        // 状态外置 g_toggleOn，点击在 WM_COMMAND 里翻转。
        HWND h = CreateWindowExW(0, L"BUTTON", L"",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 44, 24, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        g_toggleOn[h] = false;
        return h;
    };
    auto mkCombo = [&](int id, void (*fill)(HWND)) -> HWND {
        HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST,
                                 0, 0, 10, MulDiv(30, scale, 96) * comboListUnits,
                                 g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        if (fill) fill(h);
        return h;
    };
    auto mkEdit = [&](int id) -> HWND {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"9",
                               WS_CHILD | WS_TABSTOP | ES_NUMBER,
                               0, 0, 10, 30, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
    };
    auto mkBtn = [&](const wchar_t* text, int id) -> HWND {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 10, 32, g_hSettings, (HMENU)(INT_PTR)id, hInst, NULL);
        g_accentBtns.push_back(h); // 皮肤强调色按钮
        return h;
    };

    // ---- 页 1：常规 ----
    addPage(L"常规");
    addCard(L"输入");
    g_hChkPinyin = mkToggle(IDC_CHK_PINYIN);
    mkRow(L"拼音组合模式", L"字母上屏汉字，空格或数字选择；关闭则字母直通并启用英文补全",
          g_hChkPinyin, CK_TOGGLE, 44);
    g_hChkFuzzy = mkToggle(IDC_CHK_FUZZY);
    mkRow(L"模糊音", L"z/zh、c/ch、s/sh、n/l、前后鼻音互通，精确匹配优先",
          g_hChkFuzzy, CK_TOGGLE, 44);
    g_hChkShuangpin = mkToggle(IDC_CHK_SHUANGPIN);
    mkRow(L"小鹤双拼", L"每音节两键：声母+韵母，零声母按韵母首字母补齐",
          g_hChkShuangpin, CK_TOGGLE, 44);
    g_hChkVMode = mkToggle(IDC_CHK_VMODE);
    mkRow(L"v 模式", L"中文下首字母 v 直通英文，如 vhello 输出 hello",
          g_hChkVMode, CK_TOGGLE, 44);
    addCard(L"输出");
    g_hChkCnPunct = mkToggle(IDC_CHK_CNPUNCT);
    mkRow(L"中文标点", L". , / ; 等自动转全角，Shift 输出 ？：》《 等",
          g_hChkCnPunct, CK_TOGGLE, 44);
    g_hChkTraditional = mkToggle(IDC_CHK_TRADITIONAL);
    mkRow(L"繁体输出", L"候选简体字转为繁体上屏", g_hChkTraditional, CK_TOGGLE, 44);

    // ---- 页 2：按键 ----
    addPage(L"按键");
    addCard(L"切换");
    auto fillToggle = [](HWND h) {
        for (const wchar_t* s : { L"关闭", L"Shift", L"Shift 或 Ctrl", L"CapsLock", L"Ctrl" })
            ComboBox_AddString(h, s);
    };
    g_hCmbToggleMode = mkCombo(IDC_CMB_TOGGLEMODE, fillToggle);
    mkRow(L"中英切换键", L"单击切换中英文，候选窗显示中/英提示；CapsLock 模式下键盘大小写灯仍会翻转（系统底层行为），介意请用 Shift 模式",
          g_hCmbToggleMode, CK_COMBO, 150);
    addCard(L"符号");
    g_hChkSymbolHotkey = mkToggle(IDC_CHK_SYMBOLHOTKEY);
    mkRow(L"符号面板热键", L"Ctrl+Shift+B 全局独占打开符号面板，默认关闭，也可从托盘打开",
          g_hChkSymbolHotkey, CK_TOGGLE, 44);
    addCard(L"英文补全");
    g_hChkEn = mkToggle(IDC_CHK_EN);
    mkRow(L"英文逐键补全", L"词中自动补全英文单词", g_hChkEn, CK_TOGGLE, 44);
    g_hEdtCount = mkEdit(IDC_EDT_COUNT);
    mkRow(L"补全候选数", L"英文补全显示的候选数量（1-9）", g_hEdtCount, CK_EDIT, 64);

    // ---- 页 3：外观 ----
    addPage(L"外观");
    addCard(L"候选窗");
    auto fillDark = [](HWND h) {
        for (const wchar_t* s : { L"跟随应用", L"浅色", L"深色" })
            ComboBox_AddString(h, s);
    };
    g_hCmbDark = mkCombo(IDC_CMB_DARK, fillDark);
    mkRow(L"明暗", L"候选窗跟随应用主题或强制浅色/深色", g_hCmbDark, CK_COMBO, 150);
    auto fillFont = [](HWND h) {
        for (uint32_t z : {12u, 14u, 16u, 18u, 20u, 24u, 28u, 32u})
        {
            wchar_t b[8];
            _snwprintf_s(b, _TRUNCATE, L"%u", z);
            ComboBox_AddString(h, b);
        }
    };
    g_hCmbFontSize = mkCombo(IDC_CMB_FONTSIZE, fillFont);
    mkRow(L"字号", L"候选词条文字大小", g_hCmbFontSize, CK_COMBO, 100);
    auto fillSkin = [](HWND h) {
        for (const wchar_t* s : { L"翠绿", L"青碧", L"橙色", L"红色", L"金色" })
            ComboBox_AddString(h, s);
    };
    g_hCmbSkin = mkCombo(IDC_CMB_SKIN, fillSkin);
    mkRow(L"界面配色", L"高亮与强调色皮肤", g_hCmbSkin, CK_COMBO, 100);

    // ---- 页 4：词典 ----
    addPage(L"词典");
    addCard(L"词典与工具");
    HWND hBtnDict = mkBtn(L"打开词典", IDC_BTN_USERDICT);
    mkRow(L"用户词典", L"管理用户词典与自定义词条", hBtnDict, CK_BUTTON, 130);
    HWND hBtnPhrases = mkBtn(L"管理短语", IDC_BTN_HOTPHRASES);
    mkRow(L"快捷短语", L"查看和维护快捷短语", hBtnPhrases, CK_BUTTON, 130);

    // ---- 页 5：智能 ----
    addPage(L"智能");
    addCard(L"智能特性");
    g_hChkLlmRerank = mkToggle(IDC_CHK_LLMRERANK);
    mkRow(L"LLM 上下文重排", L"实验：停顿打完一字后按上文重排候选，增加 CPU 占用",
          g_hChkLlmRerank, CK_TOGGLE, 44);
    g_hChkLlmMultiPath = mkToggle(IDC_CHK_LLMMULTIPATH);
    mkRow(L"LLM 多路采样", L"实验：贪心+采样两路投票，重排更稳定，进一步增加 CPU 占用",
          g_hChkLlmMultiPath, CK_TOGGLE, 44);

    EnumChildWindows(g_hSettings, [](HWND h, LPARAM lp) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)g_hFontLabel);
    for (HWND h : g_descWnds) // 行描述：次级字号
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontDesc, TRUE);

    // 最小尺寸=左导航+右侧卡片可读下限；默认 900×680
    g_setMinW = MulDiv(700, scale, 96);
    g_setMinH = MulDiv(520, scale, 96);

    LoadToControls();
    ApplyPageVisibility(); // 只显示当前页的行控件
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
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
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

// ---------------- 候选词右键词典操作（候选窗右键菜单，2026-10-29） ----------------
// 注意：这两个函数必须保持在匿名命名空间之外（外部链接，window.cpp 引用）；
// 它们调用的 UserDictPathW/ParseUserDict/InferPinyinKey 等助手在同 TU 匿名
// 命名空间内，此处仍可见。

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

    std::string raw;
    std::vector<UdEntry> entries;
    if (ReadFileBytes(UserDictPathW(), raw)) entries = ParseUserDict(raw);

    if (op == CandDictOp::Delete)
    {
        // 按词内容匹配删除全部读音条目（引擎并发重写文件，索引不可信）
        size_t before = entries.size();
        for (size_t i = 0; i < entries.size(); )
        {
            if (entries[i].word == word) entries.erase(entries.begin() + i);
            else ++i;
        }
        if (entries.size() == before)
        {
            LX_LOG(L"ui", L"右键删除落空（基础词典词，非用户词条）: %ls", word.c_str());
            return false;
        }
    }
    else
    {
        int delta = (op == CandDictOp::Pin) ? 10000 : 1000;
        bool found = false;
        for (auto& e : entries)
        {
            if (e.word != word) continue;
            found = true;
            if (op == CandDictOp::Lower) { e.count -= 1000; if (e.count < 1) e.count = 1; }
            else e.count += delta;
        }
        if (!found)
        {
            if (op == CandDictOp::Lower)
            {
                LX_LOG(L"ui", L"右键降频落空（词条不在用户词典）: %ls", word.c_str());
                return false;
            }
            std::wstring k = deriveKey();
            if (k.empty())
            {
                LX_LOG(L"ui", L"右键新增落空（无法确定拼音键）: %ls", word.c_str());
                return false;
            }
            entries.push_back({ word, k, delta });
        }
    }
    if (!WriteFileBytes(UserDictPathW(), SerializeUserDict(entries)))
    {
        LX_LOG(L"ui", L"右键词典操作写入失败（文件被占用或无权限）: %ls", word.c_str());
        return false;
    }
    if (g_reloadUserDict) g_reloadUserDict();
    LX_LOG(L"ui", L"右键词典操作完成: op=%d word=%ls", (int)op, word.c_str());
    return true;
}

} // namespace lingxi
