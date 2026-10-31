#include "settings.h"

#include <windows.h>
#include <cwchar>

namespace lingxi {

namespace {

// 真实设置基键（用户环境）
constexpr wchar_t kKeyReal[] = L"Software\\LingxiIme";

// 测试隔离：LINGXI_SETTINGS_NS 非空（仅 [A-Za-z0-9_-]{1,16} 生效）→ 读写
// Software\LingxiIme_<ns> 沙盒基键。
// 背景（2026-10-29 晚实证）：全套测试此前直写真实 HKCU——设置保存是
// "读当前值改单键全量回写"链路的任何一环在测试进程里崩溃/半程退出都会把
// 用户实时设置刷成残值（本轮就发生过一次：全部能力位被刷 0，用户输入法
// 直接被关掉拼音）。词典有 APPDATA 沙盒、注册表此前没有等价物，现在补上。
const wchar_t* ActiveKey()
{
    wchar_t ns[64] = L"";
    DWORD n = GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", ns, 48);
    if (n == 0 || n >= 48) return kKeyReal;
    size_t len = 0;
    while (ns[len]) {
        wchar_t c = ns[len];
        bool okSym = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                     (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
        if (!okSym) return kKeyReal; // 非法字符：宁可不隔离也不写到奇怪的键
        len++;
    }
    if (len == 0) return kKeyReal;
    static wchar_t buf[128];
    _snwprintf_s(buf, _TRUNCATE, L"Software\\LingxiIme_%s", ns);
    return buf;
}

bool GetDword(HKEY hKey, const wchar_t* name, DWORD& out)
{
    DWORD v = 0, cb = sizeof(v), type = 0;
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_DWORD, &type, &v, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD)
    {
        out = v;
        return true;
    }
    return false;
}

void SetDword(HKEY hKey, const wchar_t* name, DWORD v)
{
    RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

// REG_SZ 读（上限 2KB 防异常大值；尾部 NUL 剥离）
bool GetString(HKEY hKey, const wchar_t* name, std::wstring& out)
{
    DWORD cb = 0;
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &cb) != ERROR_SUCCESS ||
        cb == 0 || cb > 2048)
        return false;
    std::wstring buf(cb / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_SZ, NULL,
                     (LPBYTE)&buf[0], &cb) != ERROR_SUCCESS)
        return false;
    size_t len = wcslen(buf.c_str()); // 数据可能带 NUL 填充，按 C 串截
    out = buf.substr(0, len);
    return true;
}

void SetString(HKEY hKey, const wchar_t* name, const std::wstring& v)
{
    RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                   (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

} // namespace

Settings LoadSettings()
{
    Settings s;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ActiveKey(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return s; // 缺键 = 全默认

    DWORD v = 0;
    if (GetDword(hKey, L"PinyinEnabled", v)) s.pinyinEnabled = v != 0;
    if (GetDword(hKey, L"CandidateCount", v) && v >= 1 && v <= 9) s.candidateCount = v;
    if (GetDword(hKey, L"CandidateFontSize", v) && v >= 12 && v <= 32) s.candidateFontSize = v;
    if (GetDword(hKey, L"DarkMode", v) && v <= 2) s.darkMode = v;
    if (GetDword(hKey, L"EnCompletion", v)) s.enCompletion = v != 0;
    if (GetDword(hKey, L"Paused", v)) s.paused = v != 0;
    if (GetDword(hKey, L"CnPunct", v)) s.cnPunct = v != 0;
    if (GetDword(hKey, L"FuzzyPinyin", v)) s.fuzzyPinyin = v != 0;
    if (GetDword(hKey, L"FuzzyGroups", v) && v <= 0xFF) s.fuzzyGroups = v; // 缺键=全组，与历史行为一致
    // 中英切换键：新键 ToggleMode 优先；无/越界则旧键 ShiftToggle
    // （bool）派生 0/1；两键均缺 = 默认 1（Shift 切换，与历史行为一致）
    DWORD tm = 0;
    if (GetDword(hKey, L"ToggleMode", tm) && tm <= 4)
        s.toggleMode = tm;
    else if (GetDword(hKey, L"ShiftToggle", v))
        s.toggleMode = v != 0 ? 1 : 0;
    if (GetDword(hKey, L"Shuangpin", v)) s.shuangpin = v != 0;
    if (GetDword(hKey, L"SymbolHotkey", v)) s.symbolHotkey = v != 0;
    if (GetDword(hKey, L"Traditional", v)) s.traditionalOutput = v != 0;
    if (GetDword(hKey, L"LlmRerank", v)) s.llmRerank = v != 0;
    if (GetDword(hKey, L"LlmMultiPath", v)) s.llmMultiPath = v != 0;
    if (GetDword(hKey, L"SkinIndex", v) && v <= 6) s.skinIndex = v;
    if (GetDword(hKey, L"UiFont", v) && v <= 3) s.uiFont = v;
    if (GetDword(hKey, L"CandidateOpacity", v) && v >= 70 && v <= 100) s.candidateOpacity = v;
    if (GetDword(hKey, L"AssocAfterPick", v)) s.assocAfterPick = (v != 0);  // 缺键=结构默认(true)
    if (GetDword(hKey, L"CandidatePinned", v)) s.candidatePinned = (v != 0 ? 1u : 0u);
    if (GetDword(hKey, L"CandidatePinX", v)) s.candidatePinX = (int)v; // 负值经 DWORD 往返
    if (GetDword(hKey, L"CandidatePinY", v)) s.candidatePinY = (int)v;
    GetString(hKey, L"RecentSymbols", s.recentSymbols); // 缺键=空（"最近"页空态）
    GetString(hKey, L"FavoriteSymbols", s.favoriteSymbols); // 缺键=空（"收藏"页空态）
    if (GetDword(hKey, L"DailyCharGoal", v) && v <= 100000) s.dailyCharGoal = v;
    if (GetDword(hKey, L"LastSettingsPage", v)) s.lastSettingsPage = v; // 越界钳制在消费方（页数随版本变）
    if (GetDword(hKey, L"LastSymbolCat", v)) s.lastSymbolCat = v;
    if (GetDword(hKey, L"SetWinX", v)) s.setWinX = (int)v; // 负值经 DWORD 往返
    if (GetDword(hKey, L"SetWinY", v)) s.setWinY = (int)v;
    if (GetDword(hKey, L"SetWinW", v)) s.setWinW = (int)v;
    if (GetDword(hKey, L"SetWinH", v)) s.setWinH = (int)v;

    RegCloseKey(hKey);
    return s;
}

void SaveSettings(const Settings& s)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ActiveKey(), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    SetDword(hKey, L"PinyinEnabled", s.pinyinEnabled ? 1 : 0);
    SetDword(hKey, L"CandidateCount", s.candidateCount);
    SetDword(hKey, L"CandidateFontSize", s.candidateFontSize);
    SetDword(hKey, L"DarkMode", s.darkMode);
    SetDword(hKey, L"EnCompletion", s.enCompletion ? 1 : 0);
    SetDword(hKey, L"Paused", s.paused ? 1 : 0);
    SetDword(hKey, L"CnPunct", s.cnPunct ? 1 : 0);
    SetDword(hKey, L"FuzzyPinyin", s.fuzzyPinyin ? 1 : 0);
    SetDword(hKey, L"FuzzyGroups", s.fuzzyGroups);
    // 双键兼容：ShiftToggle（旧 bool）与新 ToggleMode 同步写——旧版 UI/DLL 读旧键
    // 仍得正确的开/关语义（仅"Shift 或 Ctrl"细模式对旧版退化为"开"）
    SetDword(hKey, L"ShiftToggle", s.toggleMode != 0 ? 1 : 0);
    SetDword(hKey, L"ToggleMode", s.toggleMode);
    SetDword(hKey, L"Shuangpin", s.shuangpin ? 1 : 0);
    SetDword(hKey, L"SymbolHotkey", s.symbolHotkey ? 1 : 0);
    SetDword(hKey, L"Traditional", s.traditionalOutput ? 1 : 0);
    SetDword(hKey, L"LlmRerank", s.llmRerank ? 1 : 0);
    SetDword(hKey, L"LlmMultiPath", s.llmMultiPath ? 1 : 0);
    SetDword(hKey, L"SkinIndex", s.skinIndex);
    SetDword(hKey, L"UiFont", s.uiFont);
    SetDword(hKey, L"CandidateOpacity", s.candidateOpacity);
    SetDword(hKey, L"AssocAfterPick", s.assocAfterPick ? 1u : 0u);
    SetDword(hKey, L"CandidatePinned", s.candidatePinned);
    SetDword(hKey, L"CandidatePinX", (DWORD)(uint32_t)s.candidatePinX);
    SetDword(hKey, L"CandidatePinY", (DWORD)(uint32_t)s.candidatePinY);
    SetString(hKey, L"RecentSymbols", s.recentSymbols);
    SetString(hKey, L"FavoriteSymbols", s.favoriteSymbols);
    SetDword(hKey, L"DailyCharGoal", s.dailyCharGoal);
    SetDword(hKey, L"LastSettingsPage", s.lastSettingsPage);
    SetDword(hKey, L"LastSymbolCat", s.lastSymbolCat);
    SetDword(hKey, L"SetWinX", (DWORD)(uint32_t)s.setWinX);
    SetDword(hKey, L"SetWinY", (DWORD)(uint32_t)s.setWinY);
    SetDword(hKey, L"SetWinW", (DWORD)(uint32_t)s.setWinW);
    SetDword(hKey, L"SetWinH", (DWORD)(uint32_t)s.setWinH);

    RegCloseKey(hKey);
}


namespace {
// 单键 diff 写：值与当前磁盘值不同才写（写失败静默，语义同 SaveSettings）
void PutDwordDiff(HKEY hKey, const wchar_t* name, DWORD want)
{
    DWORD cur = 0;
    if (GetDword(hKey, name, cur) && cur == want) return;
    SetDword(hKey, name, want);
}

void PutStringDiff(HKEY hKey, const wchar_t* name, const std::wstring& want)
{
    std::wstring cur;
    if (GetString(hKey, name, cur) && cur == want) return;
    SetString(hKey, name, want);
}
} // namespace

void SaveSettingsDiff(const Settings& s)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ActiveKey(), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;
    PutDwordDiff(hKey, L"PinyinEnabled", s.pinyinEnabled ? 1 : 0);
    PutDwordDiff(hKey, L"CandidateCount", s.candidateCount);
    PutDwordDiff(hKey, L"CandidateFontSize", s.candidateFontSize);
    PutDwordDiff(hKey, L"DarkMode", s.darkMode);
    PutDwordDiff(hKey, L"EnCompletion", s.enCompletion ? 1 : 0);
    PutDwordDiff(hKey, L"Paused", s.paused ? 1 : 0);
    PutDwordDiff(hKey, L"CnPunct", s.cnPunct ? 1 : 0);
    PutDwordDiff(hKey, L"FuzzyPinyin", s.fuzzyPinyin ? 1 : 0);
    PutDwordDiff(hKey, L"FuzzyGroups", s.fuzzyGroups);
    PutDwordDiff(hKey, L"ShiftToggle", s.toggleMode != 0 ? 1 : 0);
    PutDwordDiff(hKey, L"ToggleMode", s.toggleMode);
    PutDwordDiff(hKey, L"Shuangpin", s.shuangpin ? 1 : 0);
    PutDwordDiff(hKey, L"SymbolHotkey", s.symbolHotkey ? 1 : 0);
    PutDwordDiff(hKey, L"Traditional", s.traditionalOutput ? 1 : 0);
    PutDwordDiff(hKey, L"LlmRerank", s.llmRerank ? 1 : 0);
    PutDwordDiff(hKey, L"LlmMultiPath", s.llmMultiPath ? 1 : 0);
    PutDwordDiff(hKey, L"SkinIndex", s.skinIndex);
    PutDwordDiff(hKey, L"UiFont", s.uiFont);
    PutDwordDiff(hKey, L"CandidateOpacity", s.candidateOpacity);
    PutDwordDiff(hKey, L"AssocAfterPick", s.assocAfterPick ? 1u : 0u);
    PutDwordDiff(hKey, L"CandidatePinned", s.candidatePinned);
    PutDwordDiff(hKey, L"CandidatePinX", (DWORD)(uint32_t)s.candidatePinX);
    PutDwordDiff(hKey, L"CandidatePinY", (DWORD)(uint32_t)s.candidatePinY);
    PutStringDiff(hKey, L"RecentSymbols", s.recentSymbols);
    PutStringDiff(hKey, L"FavoriteSymbols", s.favoriteSymbols);
    PutDwordDiff(hKey, L"DailyCharGoal", s.dailyCharGoal);
    PutDwordDiff(hKey, L"LastSettingsPage", s.lastSettingsPage);
    PutDwordDiff(hKey, L"LastSymbolCat", s.lastSymbolCat);
    PutDwordDiff(hKey, L"SetWinX", (DWORD)(uint32_t)s.setWinX);
    PutDwordDiff(hKey, L"SetWinY", (DWORD)(uint32_t)s.setWinY);
    PutDwordDiff(hKey, L"SetWinW", (DWORD)(uint32_t)s.setWinW);
    PutDwordDiff(hKey, L"SetWinH", (DWORD)(uint32_t)s.setWinH);
    RegCloseKey(hKey);
}

void ResetSettingsToDefaults()
{
    // RegDeleteTreeW 连键删除（比逐值枚举删除简单且原子）；下次保存时
    // RegCreateKeyExW 自动重建，LoadSettings 走"键不存在=全默认"路径。
    RegDeleteTreeW(HKEY_CURRENT_USER, ActiveKey());
}
} // namespace lingxi
