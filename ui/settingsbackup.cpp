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

// L6：三态返回——超时强杀与真失败分开，调用方据此给准确提示
enum class CmdResult { Ok, Failed, Timeout };

CmdResult RunCmdHidden(const std::wstring& args)
{
    std::wstring cmd = L"cmd.exe " + args;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    // L6：子进程挂"关闭即杀"Job——TerminateProcess 只杀 cmd.exe 本身，
    // 孙进程 reg.exe 会成孤儿后台续跑（正是审计"可能覆盖后续导入"的根因），
    // 关 Job 句柄即清整棵进程树。挂起创建、指派后再恢复，防指派前 cmd
    // 已派生 reg.exe；指派失败（宿主已在不支持嵌套的 Job 内）退回只杀直子。
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi))
    {
        if (job) CloseHandle(job);
        return CmdResult::Failed;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    DWORD wait = WaitForSingleObject(pi.hProcess, 15000);
    // L6：超时强杀收敛——reg.exe 罕见挂起（hive 锁/系统抖动）时旧实现拿
    // STILL_ACTIVE(259) 当退出码判 false 但进程句柄泄漏、cmd 永挂；强杀
    // 后再等 3s 保证直子退出，句柄全关。超时分支以 timedOut 为准判
    // Timeout，不依赖强杀后的退出码取值。
    const bool timedOut = (wait != WAIT_OBJECT_0);
    if (timedOut)
    {
        TerminateProcess(pi.hProcess, (UINT)-1);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job); // 关 Job 连带收掉残存孙进程（reg.exe）
    if (timedOut) return CmdResult::Timeout;
    return code == 0 ? CmdResult::Ok : CmdResult::Failed;
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
    CmdResult res = RunCmdHidden(args);
    const bool ok = (res == CmdResult::Ok);
    // 用户词典 + 热键短语随行（同 basename"-用户词典.txt"/"-热键短语.txt"）：
    // 两者都在 APPDATA 文本文件，注册表备份不含；一并导出让备份完整可迁移。
    // 源文件不存在=正常（新装）；存在但读/写失败=备份不完整，必须告知用户
    //（审计轮 7 修复：原实现仅记日志，弹窗仍报"已导出"，虚假信心）。
    int sidecarFail = 0;
    bool dictOk = false, hpOk = false, blOk = false;
    if (ok)
    {
        std::wstring base(path);
        size_t dot = base.rfind(L'.');
        if (dot != std::wstring::npos) base = base.substr(0, dot);
        std::string raw;
        // L7：导出前先收敛在途学习——用户刚打的词若还在内存未落盘（学习
        // 线程批量刷盘周期内），备份文件缺最新词条
        if (g_hooks.flushUserDict) g_hooks.flushUserDict();
        if (ReadFileBytes(UserDictPathW(), raw))
        {
            if (WriteFileBytes(base + L"-用户词典.txt", raw)) dictOk = true;
            else sidecarFail++;
        }
        if (ReadFileBytes(HotPhraseDataPath(), raw))
        {
            if (WriteFileBytes(base + L"-热键短语.txt", raw)) hpOk = true;
            else sidecarFail++;
        }
        // 屏蔽词随行（"-屏蔽词.txt"）：blocked.txt 也在 APPDATA，注册表备份不含
        if (ReadFileBytes(BlockedWordsPathW(), raw))
        {
            if (WriteFileBytes(base + L"-屏蔽词.txt", raw)) blOk = true;
            else sidecarFail++;
        }
    }
    LX_LOG(L"ui", L"设置备份导出 res=%d dict=%d hp=%d bl=%d fail=%d path=%ls",
           (int)res, (int)dictOk, (int)hpOk, (int)blOk, sidecarFail, path);
    if (ok && sidecarFail)
        MessageBoxW(owner,
                    (L"注册表设置已导出，但 " + std::to_wstring(sidecarFail) +
                     L" 个随行数据文件（词典/短语/屏蔽词）写入失败，备份不完整！\n"
                     L"详情见日志，请检查目标路径后重试。").c_str(),
                    L"导出设置备份", MB_OK | MB_ICONWARNING);
    else if (res == CmdResult::Timeout)
        MessageBoxW(owner,
                    L"导出失败：reg.exe 超时已强制结束（备份文件可能不完整，请删除后重试）。",
                    L"导出设置备份", MB_OK | MB_ICONWARNING);
    else if (!ok)
        MessageBoxW(owner, L"导出失败（路径不可写或 reg.exe 不可用）。",
                    L"导出设置备份", MB_OK | MB_ICONWARNING);
    else
        MessageBoxW(owner, L"设置备份已导出。",
                    L"导出设置备份", MB_OK | MB_ICONINFORMATION);
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
    CmdResult res = RunCmdHidden(args);
    const bool ok = (res == CmdResult::Ok);
    bool dictRestored = false, hpRestored = false, blRestored = false;
    int restoreFail = 0; // 用户已确认恢复但读/写失败（审计轮 7：须告知，防"以为已还原"）
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
                // 走引擎原子操作（清空+合并），不再直写文件：与后台落盘
                // 竞态会复活旧词条（见 UserDictReplaceFromRaw）
                if (ReadFileBytes(dictSrc, raw) && UserDictReplaceFromRaw(raw))
                    dictRestored = true;
                else restoreFail++;
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
                else restoreFail++;
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
                else restoreFail++;
            }
        }
    }
    LX_LOG(L"ui", L"设置备份导入 res=%d dict=%d hp=%d bl=%d fail=%d path=%ls",
           (int)res, (int)dictRestored, (int)hpRestored, (int)blRestored, restoreFail, path);
    if (ok && restoreFail)
        MessageBoxW(owner,
                    (L"注册表设置已导入，但 " + std::to_wstring(restoreFail) +
                     L" 项数据（词典/短语/屏蔽词）恢复失败，当前文件未被覆盖！\n"
                     L"详情见日志，请检查文件占用后重试。").c_str(),
                    L"导入设置备份", MB_OK | MB_ICONWARNING);
    else if (res == CmdResult::Timeout)
        MessageBoxW(owner,
                    L"导入失败：reg.exe 超时已强制结束（设置可能被部分改动，请确认后重试）。",
                    L"导入设置备份", MB_OK | MB_ICONWARNING);
    else if (!ok)
        MessageBoxW(owner, L"导入失败（文件不是有效的注册表导出格式？）。",
                    L"导入设置备份", MB_OK | MB_ICONWARNING);
    else
        MessageBoxW(owner, L"设置已导入，即时生效（设置页若开着请重开刷新）。",
                    L"导入设置备份", MB_OK | MB_ICONINFORMATION);
}

} // namespace lingxi
