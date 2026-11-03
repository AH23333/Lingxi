//
// ===== 设置页"更新"区接线（批次 5）实现 =====
// 状态机与设计 §5 对齐；worker 线程做检查/下载/校验/暂存/提权发起，
// UI 线程只收 WM_UPD_* 消息刷新（T11：不阻塞、可取消、失败必附原因）。
//
#include "updateui.h"

#include <windowsx.h> // Edit_SetText / Edit_GetText 宏
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "../common/update.h"
#include "../common/update_fetch.h"
#include "../common/update_apply.h"
#include "../common/version.h"

namespace lingxi {
namespace {

using lingxi::updater::ApplyHooks;
using lingxi::updater::ApplyRequest;
using lingxi::updater::CheckLatest;
using lingxi::updater::ComputeBinRootsFromModule;
using lingxi::updater::DownloadToFile;
using lingxi::updater::ExtractZipTo;
using lingxi::updater::kMaxDownloadBytes;
using lingxi::updater::LaunchElevatedApply;
using lingxi::updater::ParseSha256Sums;
using lingxi::updater::PreflightZip;
using lingxi::updater::ReleaseInfo;
using lingxi::updater::SelectedAssets;
using lingxi::updater::SemVer;
using lingxi::updater::State;
using lingxi::updater::StateText;
using lingxi::updater::Utf8ToWide;
using lingxi::updater::ValidateStagingPath;
using lingxi::updater::VerifyAgainstSums;
using lingxi::updater::VerifyExtractedDir;
using lingxi::updater::ZipEntry;

UpdateCtl g_c;
std::wstring g_binRoot, g_workRoot;
std::wstring g_detail;                 // 当前状态的附加原因
std::wstring g_verStr;                 // 远端新版本串（如 1.2.3）
ReleaseInfo g_rel;                     // 检查得到的 release（ApplyWorker 用）
bool g_relValid = false;
int g_state = (int)State::Idle;
volatile LONG g_cancel = 0;
HANDLE g_worker = nullptr;

// ---------- 小工具 ----------
bool MkdirP(const std::wstring& p)
{
    size_t pos = (p.size() >= 2 && p[1] == L':') ? 3 : 0;
    for (;;) {
        size_t next = p.find(L'\\', pos);
        std::wstring seg = (next == std::wstring::npos) ? p : p.substr(0, next);
        if (!seg.empty()) {
            DWORD attr = GetFileAttributesW(seg.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES &&
                !CreateDirectoryW(seg.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        if (next == std::wstring::npos) return true;
        pos = next + 1;
    }
}

bool ReadFileBytes(const std::wstring& path, std::string* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    out->clear();
    char buf[4096];
    DWORD got;
    while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0) {
        out->append(buf, got);
        if (out->size() > (1u << 20)) break;
    }
    CloseHandle(h);
    return true;
}

std::wstring VerStr(const SemVer& v)
{
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%u.%u.%u", (unsigned)v.major, (unsigned)v.minor,
                 (unsigned)v.patch);
    return buf;
}

std::wstring LastCheckText(unsigned long long unixSec)
{
    if (unixSec == 0) return L"从未检查";
    time_t t = (time_t)unixSec;
    struct tm tmv {};
    localtime_s(&tmv, &t);
    wchar_t buf[64];
    _snwprintf_s(buf, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                 tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}

bool WorkerAlive()
{
    return g_worker && WaitForSingleObject(g_worker, 0) == WAIT_TIMEOUT;
}

void PostState(State st, std::wstring* detail)
{
    g_state = (int)st;
    if (detail) {
        g_detail = *detail;
        if (g_c.ownerWnd)
            PostMessageW(g_c.ownerWnd, WM_UPD_STATE, (WPARAM)st, (LPARAM)detail);
        else
            delete detail;
    } else {
        g_detail.clear();
        if (g_c.ownerWnd)
            PostMessageW(g_c.ownerWnd, WM_UPD_STATE, (WPARAM)st, (LPARAM) nullptr);
    }
}

void StartWorker(LPTHREAD_START_ROUTINE fn)
{
    if (WorkerAlive()) return;
    InterlockedExchange(&g_cancel, 0);
    HANDLE h = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (g_worker) CloseHandle(g_worker);
    g_worker = h;
    if (!h) PostState(State::Failed, new std::wstring(L"无法启动后台任务"));
}

// ---------- 检查 worker ----------
DWORD WINAPI CheckWorker(LPVOID)
{
    Settings s = LoadSettings();
    ReleaseInfo rel;
    SemVer remote;
    std::wstring err;
    State st = CheckLatest(s.updateRepo, &g_cancel, &rel, &remote, &err);
    if (st == State::NewVersion) {
        g_rel = rel;
        g_verStr = VerStr(remote);
        g_relValid = true;
    } else {
        g_relValid = false;
    }
    if (st == State::UpToDate || st == State::NewVersion || st == State::NotPublished) {
        Settings s2 = LoadSettings();
        s2.updateLastCheck = (unsigned long long)time(NULL);
        SaveSettingsDiff(s2);
    }
    PostState(st, new std::wstring(err));
    return 0;
}

// ---------- 下载+校验+暂存+提权发起 worker ----------
bool ProgCb(long long done, long long total, void* ctx)
{
    if (g_c.ownerWnd) {
        int pct = total > 0 ? (int)(done * 100 / total) : -1;
        PostMessageW(g_c.ownerWnd, WM_UPD_PROGRESS, (WPARAM)pct,
                     (LPARAM) new std::wstring((const wchar_t*)ctx));
    }
    return InterlockedOr(&g_cancel, 0) == 0;
}

void FailStage(const std::wstring& reason)
{
    PostState(State::Failed, new std::wstring(reason));
}

DWORD WINAPI ApplyWorker(LPVOID)
{
    std::wstring err;
    SelectedAssets sel;
    if (!g_relValid || !SelectAssets(g_rel, &sel, &err)) { FailStage(err); return 0; }
    std::wstring dl = g_workRoot + L"\\download";
    if (!MkdirP(g_workRoot) || !MkdirP(dl)) { FailStage(L"无法创建 work/download 目录"); return 0; }

    std::wstring sumsPath = dl + L"\\SHA256SUMS.txt";
    PostState(State::Downloading, new std::wstring(L"校验文件"));
    if (!DownloadToFile(sel.sums.downloadUrl, sumsPath, 65536, &g_cancel, NULL, NULL, &err)) {
        FailStage(L"获取 SHA256SUMS 失败：" + err);
        return 0;
    }
    std::string sumsText;
    if (!ReadFileBytes(sumsPath, &sumsText)) { FailStage(L"读取 SHA256SUMS 失败"); return 0; }
    std::vector<std::pair<std::wstring, std::string>> sums;
    if (!ParseSha256Sums(sumsText, &sums, &err)) { FailStage(err); return 0; }

    std::wstring z64 = dl + L"\\" + sel.x64.name;
    std::wstring z86 = dl + L"\\" + sel.x86.name;
    if (!DownloadToFile(sel.x64.downloadUrl, z64, kMaxDownloadBytes, &g_cancel, ProgCb,
                        (void*)L"x64 包", &err)) {
        FailStage(L"下载 x64 失败：" + err);
        return 0;
    }
    if (!DownloadToFile(sel.x86.downloadUrl, z86, kMaxDownloadBytes, &g_cancel, ProgCb,
                        (void*)L"x86 包", &err)) {
        FailStage(L"下载 x86 失败：" + err);
        return 0;
    }

    PostState(State::Verifying, new std::wstring(L"SHA256 比对"));
    bool ok64 = VerifyAgainstSums(sums, sel.x64.name, z64, &err);
    bool ok86 = ok64 && VerifyAgainstSums(sums, sel.x86.name, z86, &err);
    if (!ok64 || !ok86) {
        DeleteFileW(z64.c_str());
        DeleteFileW(z86.c_str()); // 哈希不匹配 = 整包丢弃，绝不替换（T1）
        FailStage(L"校验失败（整包已丢弃）：" + err);
        return 0;
    }
    std::vector<ZipEntry> ents;
    if (!PreflightZip(z64, &ents, &err)) { FailStage(L"x64 包预校验失败：" + err); return 0; }
    if (!PreflightZip(z86, &ents, &err)) { FailStage(L"x86 包预校验失败：" + err); return 0; }

    std::wstring staging = g_workRoot + L"\\staging\\" + g_verStr;
    std::error_code ec;
    std::filesystem::remove_all(staging, ec); // 幂等：旧暂存一律重建
    if (!MkdirP(staging + L"\\x64") || !MkdirP(staging + L"\\x86")) {
        FailStage(L"无法创建 staging 目录");
        return 0;
    }
    if (!ExtractZipTo(z64, staging + L"\\x64", &g_cancel, &err) ||
        !ExtractZipTo(z86, staging + L"\\x86", &g_cancel, &err)) {
        FailStage(L"解压失败：" + err);
        return 0;
    }
    if (!VerifyExtractedDir(staging + L"\\x64", &err) ||
        !VerifyExtractedDir(staging + L"\\x86", &err)) {
        std::filesystem::remove_all(staging, ec);
        FailStage(L"解出复核失败：" + err);
        return 0;
    }
    SemVer vv;
    if (!ValidateStagingPath(staging, g_workRoot, &vv, &err)) {
        FailStage(L"staging 自检失败：" + err);
        return 0;
    }

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
    std::wstring resultFile = g_workRoot + L"\\apply-result.txt";
    DeleteFileW(resultFile.c_str());
    PostState(State::Staged, new std::wstring(L"校验与暂存完成，即将请求一次 UAC 提权"));
    if (!LaunchElevatedApply(self, staging, &err)) { FailStage(err); return 0; }
    PostState(State::Replacing, new std::wstring(L"等待提权进程完成（UAC 确认后自动进行）"));

    for (int i = 0; i < 600; i++) { // 至多 10 分钟轮询提权侧结果（T12：独立复核文件，不猜）
        if (InterlockedOr(&g_cancel, 0)) {
            FailStage(L"已取消等待（提权进程可能仍在进行，以 apply-result 为准）");
            return 0;
        }
        std::string content;
        if (ReadFileBytes(resultFile, &content)) {
            std::wstring w = Utf8ToWide(content);
            size_t nl = w.find(L'\n');
            std::wstring first = w.substr(0, nl == std::wstring::npos ? w.size() : nl);
            if (first.rfind(L"OK", 0) == 0) {
                PostState(State::AwaitingRestart,
                          new std::wstring(L"磁盘与注册表已是新版；重启宿主程序或注销重登后生效"
                                           L"（自查：%TEMP%\\lingxi-ime.log 的 build= 行）"));
            } else {
                size_t sp = first.find(L' ');
                std::wstring reason = sp == std::wstring::npos ? first : first.substr(sp + 1);
                PostState(State::Failed, new std::wstring(reason));
            }
            return 0;
        }
        Sleep(1000);
    }
    FailStage(L"提权进程 10 分钟未回报结果（可能取消了 UAC）");
    return 0;
}

// ---------- 渲染 ----------
void RenderStatus()
{
    if (g_c.statusLab) {
        std::wstring t = StateText((State)g_state);
        if (!g_detail.empty()) t += L"：" + g_detail;
        SetWindowTextW(g_c.statusLab, t.c_str());
    }
    if (g_c.btnApply)
        EnableWindow(g_c.btnApply, g_state == (int)State::NewVersion && g_relValid && !WorkerAlive());
    if (g_c.btnCheck) EnableWindow(g_c.btnCheck, !WorkerAlive());
    if (g_c.lastLab) {
        std::wstring t = L"上次检查：";
        t += LastCheckText(LoadSettings().updateLastCheck);
        SetWindowTextW(g_c.lastLab, t.c_str());
    }
}

} // namespace

// ---------- 对外 ----------
void UpdateUiAttach(const UpdateCtl& ctl)
{
    g_c = ctl;
    if (g_binRoot.empty()) ComputeBinRootsFromModule(&g_binRoot, &g_workRoot);
    Settings s = LoadSettings();
    UpdateUiSyncFromSettings(s);
    RenderStatus();
}

void UpdateUiSyncFromSettings(const Settings& s)
{
    if (g_c.repoEdit) Edit_SetText(g_c.repoEdit, s.updateRepo.c_str());
    if (g_c.autoChk && g_c.setToggle) g_c.setToggle(g_c.autoChk, s.updateAutoCheck != 0);
    if (g_c.verLab) {
        wchar_t buf[128];
        _snwprintf_s(buf, _TRUNCATE, L"灵犀输入法 %u.%u.%u（构建于 %hs）",
                     (unsigned)LINGXI_VERSION_MAJOR, (unsigned)LINGXI_VERSION_MINOR,
                     (unsigned)LINGXI_VERSION_PATCH, __DATE__);
        SetWindowTextW(g_c.verLab, buf);
    }
    if (g_c.lastLab) {
        std::wstring t = L"上次检查：";
        t += LastCheckText(s.updateLastCheck);
        SetWindowTextW(g_c.lastLab, t.c_str());
    }
    RenderStatus();
}

void UpdateUiApplyToSettings(Settings& s)
{
    if (g_c.repoEdit) {
        wchar_t buf[256] = {};
        Edit_GetText(g_c.repoEdit, buf, 256);
        s.updateRepo = buf;
    }
    if (g_c.autoChk && g_c.getToggle) s.updateAutoCheck = g_c.getToggle(g_c.autoChk) ? 1u : 0u;
}

void UpdateUiOnCheckButton()
{
    PostState(State::Checking, new std::wstring(L"正在检查 GitHub Releases…"));
    RenderStatus();
    StartWorker(CheckWorker);
}

void UpdateUiOnApplyButton()
{
    if (g_state != (int)State::NewVersion || !g_relValid || WorkerAlive()) return;
    StartWorker(ApplyWorker);
}

void UpdateUiHandleMsg(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    (void)hwnd;
    std::wstring* d = (std::wstring*)l;
    if (msg == WM_UPD_STATE) {
        g_state = (int)w;
        if (d) {
            g_detail = *d;
            delete d;
        } else {
            g_detail.clear();
        }
        RenderStatus();
    } else if (msg == WM_UPD_PROGRESS) {
        if (g_c.statusLab && d) {
            std::wstring t = L"下载中 ";
            t += *d;
            int pct = (int)(INT_PTR)w;
            if (pct >= 0) {
                wchar_t pb[16];
                _snwprintf_s(pb, _TRUNCATE, L" %d%%", pct);
                t += pb;
            }
            SetWindowTextW(g_c.statusLab, t.c_str());
        }
        delete d;
    }
}

void UpdateUiAutoCheckOnStartup()
{
    Settings s = LoadSettings();
    if (!s.updateAutoCheck || s.updateRepo.empty()) return; // 默认关；未配置=正常态不检查
    unsigned long long now = (unsigned long long)time(NULL);
    if (s.updateLastCheck != 0 && now - s.updateLastCheck < 86400) return; // 每天至多一次
    StartWorker(CheckWorker);
}

void UpdateUiShutdown()
{
    InterlockedExchange(&g_cancel, 1);
    if (WorkerAlive() && g_worker) WaitForSingleObject(g_worker, 3000);
}

} // namespace lingxi
