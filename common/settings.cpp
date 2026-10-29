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
    // 中英切换键：新键 ToggleMode 优先；无/越界则旧键 ShiftToggle
    // （bool）派生 0/1；两键均缺 = 默认 1（Shift 切换，与历史行为一致）
    DWORD tm = 0;
    if (GetDword(hKey, L"ToggleMode", tm) && tm <= 4)
        s.toggleMode = tm;
    else if (GetDword(hKey, L"ShiftToggle", v))
        s.toggleMode = v != 0 ? 1 : 0;
    if (GetDword(hKey, L"VMode", v)) s.vMode = v != 0;
    if (GetDword(hKey, L"Shuangpin", v)) s.shuangpin = v != 0;
    if (GetDword(hKey, L"SymbolHotkey", v)) s.symbolHotkey = v != 0;
    if (GetDword(hKey, L"Traditional", v)) s.traditionalOutput = v != 0;
    if (GetDword(hKey, L"LlmRerank", v)) s.llmRerank = v != 0;
    if (GetDword(hKey, L"LlmMultiPath", v)) s.llmMultiPath = v != 0;
    if (GetDword(hKey, L"SkinIndex", v) && v <= 4) s.skinIndex = v;

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
    // 双键兼容：ShiftToggle（旧 bool）与新 ToggleMode 同步写——旧版 UI/DLL 读旧键
    // 仍得正确的开/关语义（仅"Shift 或 Ctrl"细模式对旧版退化为"开"）
    SetDword(hKey, L"ShiftToggle", s.toggleMode != 0 ? 1 : 0);
    SetDword(hKey, L"ToggleMode", s.toggleMode);
    SetDword(hKey, L"VMode", s.vMode ? 1 : 0);
    SetDword(hKey, L"Shuangpin", s.shuangpin ? 1 : 0);
    SetDword(hKey, L"SymbolHotkey", s.symbolHotkey ? 1 : 0);
    SetDword(hKey, L"Traditional", s.traditionalOutput ? 1 : 0);
    SetDword(hKey, L"LlmRerank", s.llmRerank ? 1 : 0);
    SetDword(hKey, L"LlmMultiPath", s.llmMultiPath ? 1 : 0);
    SetDword(hKey, L"SkinIndex", s.skinIndex);

    RegCloseKey(hKey);
}

} // namespace lingxi
