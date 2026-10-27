#include "settings.h"

#include <windows.h>

namespace lingxi {

namespace {

constexpr wchar_t kKey[] = L"Software\\LingxiIme";

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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return s; // 缺键 = 全默认

    DWORD v = 0;
    if (GetDword(hKey, L"PinyinEnabled", v)) s.pinyinEnabled = v != 0;
    if (GetDword(hKey, L"CandidateCount", v) && v >= 1 && v <= 9) s.candidateCount = v;
    if (GetDword(hKey, L"DarkMode", v) && v <= 2) s.darkMode = v;
    if (GetDword(hKey, L"EnCompletion", v)) s.enCompletion = v != 0;
    if (GetDword(hKey, L"Paused", v)) s.paused = v != 0;
    if (GetDword(hKey, L"CnPunct", v)) s.cnPunct = v != 0;
    if (GetDword(hKey, L"FuzzyPinyin", v)) s.fuzzyPinyin = v != 0;

    RegCloseKey(hKey);
    return s;
}

void SaveSettings(const Settings& s)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    SetDword(hKey, L"PinyinEnabled", s.pinyinEnabled ? 1 : 0);
    SetDword(hKey, L"CandidateCount", s.candidateCount);
    SetDword(hKey, L"DarkMode", s.darkMode);
    SetDword(hKey, L"EnCompletion", s.enCompletion ? 1 : 0);
    SetDword(hKey, L"Paused", s.paused ? 1 : 0);
    SetDword(hKey, L"CnPunct", s.cnPunct ? 1 : 0);
    SetDword(hKey, L"FuzzyPinyin", s.fuzzyPinyin ? 1 : 0);

    RegCloseKey(hKey);
}

} // namespace lingxi
