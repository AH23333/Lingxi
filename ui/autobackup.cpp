//
// 每日自动备份实现。自包含文件 IO（usagestats 同模式，不依赖 uiinternal）。
// 多实例安全：单实例宿主（命名互斥量）保证仅一个进程走打字备份路径；
// 托盘"立即备份"与打字路径经 g_mtx 串行。
//
#include "autobackup.h"

#include <windows.h>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

namespace lingxi {

namespace {

constexpr size_t kKeepDays = 14; // daily-* 目录保留份数

std::mutex g_mtx;
uint32_t g_doneDay = 0; // 本进程已备份日期（0=尚未备份/未知）
bool g_sandbox = false;
bool g_checked = false; // 沙箱探测只做一次

void EnsureSandboxChecked()
{
    if (g_checked) return;
    g_checked = true;
    wchar_t ns[64] = {};
    if (GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", ns, 64) > 0)
        g_sandbox = true;
}

std::wstring DataDirW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::wstring(buf) + L"\\LingxiIme";
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe);
}

uint32_t TodayDayKey()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return (uint32_t)st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

std::wstring TodaySuffix()
{
    wchar_t day[16];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    _snwprintf_s(day, _TRUNCATE, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return day;
}

bool RemoveDirRecursive(const std::wstring& dir)
{
    std::wstring findDir = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(findDir.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirRecursive(p);
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

// 轮换：daily-YYYYMMDD 目录名字典序=时间序，保留最新 kKeepDays 个
void RotateOldBackups(const std::wstring& root)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\daily-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> names;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (names.size() <= kKeepDays) return;
    std::sort(names.begin(), names.end());
    size_t removeCount = names.size() - kKeepDays;
    for (size_t i = 0; i < removeCount; i++)
        RemoveDirRecursive(root + L"\\" + names[i]);
}

// 备份全部数据文件到 dayDir（存在才拷；FALSE=覆盖式当日幂等）。
// 返回：至少成功拷贝一个文件且无失败。
bool BackupInto(const std::wstring& dayDir)
{
    CreateDirectoryW(dayDir.c_str(), NULL);
    static const wchar_t* kFiles[] = {
        L"userdict.txt", L"blocked.txt", L"phrases.txt",
        L"hotphrases.txt", L"usage.txt",
    };
    const std::wstring dataDir = DataDirW();
    bool any = false, ok = true;
    for (const wchar_t* f : kFiles)
    {
        std::wstring src = dataDir + L"\\" + f;
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue; // 未用过的数据文件跳过
        if (CopyFileW(src.c_str(), (dayDir + L"\\" + f).c_str(), FALSE))
            any = true;
        else
            ok = false; // 个别文件被占用不视为整体失败，但如实上报
    }
    return any && ok;
}

} // namespace

std::wstring BackupsDirW()
{
    return DataDirW() + L"\\backups";
}

bool AutoBackupNow()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    EnsureSandboxChecked();
    if (g_sandbox) return false;
    const std::wstring root = BackupsDirW();
    CreateDirectoryW(root.c_str(), NULL);
    bool ok = BackupInto(root + L"\\daily-" + TodaySuffix());
    RotateOldBackups(root);
    if (ok) g_doneDay = TodayDayKey();
    return ok;
}

void AutoBackupTick()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    EnsureSandboxChecked();
    if (g_sandbox) return;
    uint32_t today = TodayDayKey();
    if (g_doneDay == today) return; // 高频路径短路：今日已备
    // 进程重启后首次：当日目录已存在（更早实例备的）则直接认账，幂等
    std::wstring dayDir = BackupsDirW() + L"\\daily-" + TodaySuffix();
    if (g_doneDay == 0 &&
        GetFileAttributesW(dayDir.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        g_doneDay = today;
        return;
    }
    const std::wstring root = BackupsDirW();
    CreateDirectoryW(root.c_str(), NULL);
    if (BackupInto(dayDir))
        g_doneDay = today;
    RotateOldBackups(root);
}

} // namespace lingxi
