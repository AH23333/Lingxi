//
// 设置备份（注册表导出/导入）。原 settingsui.cpp 备份段拆出（P1 重构）。
// reg.exe export/import 整个 HKCU\Software\LingxiIme 键（全部设置+热短语）。
// 用户词典/热键短语在 APPDATA 文本文件，注册表备份不含——导出时随行
// （同 basename"-用户词典.txt"/"-热键短语.txt"），导入时询问恢复，让
// 备份单文件化完整可迁移。
//
#include "uiinternal.h"

#include <windows.h>
#include <commdlg.h>
#include <string>

#include "../common/log.h"
#include "blockedui.h"
#include "hotphraseui.h"

namespace lingxi {

namespace {

bool RunCmdHidden(const std::wstring& args)
{
    std::wstring cmd = L"cmd.exe " + args;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

} // namespace

void ExportSettingsBackup(HWND owner)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"LingxiIme-设置备份-%04u%02u%02u.reg",
                 st.wYear, st.wMonth, st.wDay);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"注册表文件 (*.reg)\0*.reg\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"reg";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::wstring args = L"/c reg export \"HKCU\\Software\\LingxiIme\" \"" +
                        std::wstring(path) + L"\" /y";
    bool ok = RunCmdHidden(args);
    // 用户词典 + 热键短语随行（同 basename"-用户词典.txt"/"-热键短语.txt"）：
    // 两者都在 APPDATA 文本文件，注册表备份不含；一并导出让备份完整可迁移。
    bool dictOk = false, hpOk = false, blOk = false;
    if (ok)
    {
        std::wstring base(path);
        size_t dot = base.rfind(L'.');
        if (dot != std::wstring::npos) base = base.substr(0, dot);
        std::string raw;
        if (ReadFileBytes(UserDictPathW(), raw))
        {
            WriteFileBytes(base + L"-用户词典.txt", raw);
            dictOk = true;
        }
        if (ReadFileBytes(HotPhraseDataPath(), raw))
        {
            WriteFileBytes(base + L"-热键短语.txt", raw);
            hpOk = true;
        }
        // 屏蔽词随行（"-屏蔽词.txt"）：blocked.txt 也在 APPDATA，注册表备份不含
        if (ReadFileBytes(BlockedWordsPathW(), raw))
        {
            WriteFileBytes(base + L"-屏蔽词.txt", raw);
            blOk = true;
        }
    }
    LX_LOG(L"ui", L"设置备份导出 ok=%d dict=%d hp=%d bl=%d path=%ls",
           (int)ok, (int)dictOk, (int)hpOk, (int)blOk, path);
    MessageBoxW(owner,
                ok ? L"设置备份已导出。" : L"导出失败（路径不可写或 reg.exe 不可用）。",
                L"导出设置备份",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

void ImportSettingsBackup(HWND owner)
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"注册表文件 (*.reg)\0*.reg\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    if (MessageBoxW(owner,
                    L"将用备份覆盖当前全部设置（切换键/皮肤/热键/候选等），确定继续？",
                    L"导入设置备份", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    std::wstring args = L"/c reg import \"" + std::wstring(path) + L"\"";
    bool ok = RunCmdHidden(args);
    bool dictRestored = false, hpRestored = false, blRestored = false;
    if (ok)
    {
        if (g_hooks.settingsChanged) g_hooks.settingsChanged(); // 热键等需随新设置重注册
        UpdateTrayTip();
        std::wstring base(path);
        size_t dot = base.rfind(L'.');
        if (dot != std::wstring::npos) base = base.substr(0, dot);
        // 随行用户词典存在则询问恢复（覆盖当前词典 → 引擎重载）
        std::wstring dictSrc = base + L"-用户词典.txt";
        if (GetFileAttributesW(dictSrc.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            if (MessageBoxW(owner,
                            L"备份中含用户词典，一并恢复（覆盖当前词典）？",
                            L"导入设置备份", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            {
                std::string raw;
                if (ReadFileBytes(dictSrc, raw) &&
                    WriteFileBytes(UserDictPathW(), raw))
                {
                    if (g_hooks.reloadUserDict) g_hooks.reloadUserDict();
                    dictRestored = true;
                }
            }
        }
        // 随行热键短语存在则询问恢复（覆盖后通知重注册全部短语热键）
        std::wstring hpSrc = base + L"-热键短语.txt";
        if (GetFileAttributesW(hpSrc.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            if (MessageBoxW(owner,
                            L"备份中含热键短语，一并恢复（覆盖当前短语）？",
                            L"导入设置备份", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            {
                std::string rawHp;
                if (ReadFileBytes(hpSrc, rawHp) &&
                    WriteFileBytes(HotPhraseDataPath(), rawHp))
                {
                    NotifyHotPhrasesChanged();
                    hpRestored = true;
                }
            }
        }
        // 随行屏蔽词存在则询问恢复（blocked.txt 与用户词典同通道热载）
        std::wstring blSrc = base + L"-屏蔽词.txt";
        if (GetFileAttributesW(blSrc.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            if (MessageBoxW(owner,
                            L"备份中含屏蔽词，一并恢复（覆盖当前屏蔽词）？",
                            L"导入设置备份", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            {
                std::string rawBl;
                if (ReadFileBytes(blSrc, rawBl) &&
                    WriteFileBytes(BlockedWordsPathW(), rawBl))
                {
                    if (g_hooks.reloadUserDict) g_hooks.reloadUserDict();
                    blRestored = true;
                }
            }
        }
    }
    LX_LOG(L"ui", L"设置备份导入 ok=%d dict=%d hp=%d bl=%d path=%ls",
           (int)ok, (int)dictRestored, (int)hpRestored, (int)blRestored, path);
    MessageBoxW(owner,
                ok ? L"设置已导入，即时生效（设置页若开着请重开刷新）。"
                   : L"导入失败（文件不是有效的注册表导出格式？）。",
                L"导入设置备份",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

} // namespace lingxi
