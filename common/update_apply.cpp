//
// ===== 自助更新：替换 / 自验 / 回滚（批次 4）实现 =====
// 设计文档：docs/update-design-2026-11-03.md §6；威胁模型 T9/T10/T12。
//
#include "update_apply.h"

#include <shellapi.h>

#include <ctime>

#include "update_fetch.h"

namespace lingxi {
namespace updater {

// ---------- 路径小工具 ----------
static std::wstring NormalizeDir(const std::wstring& p)
{
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)ARRAYSIZE(buf), buf, NULL);
    if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
    std::wstring s = buf;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// 确保 path 的父目录链存在；某段已存在但是文件 → false（半替换防线）。
static bool EnsureParentDirs(const std::wstring& path, std::wstring* err)
{
    size_t sep = path.find_last_of(L'\\');
    if (sep == std::wstring::npos || sep == 0) return true;
    std::wstring dir = path.substr(0, sep);
    size_t pos = 0;
    // 从盘符之后逐段建（"C:\a\b" 从 pos=3 起）
    if (dir.size() >= 2 && dir[1] == L':') pos = 3;
    for (;;) {
        size_t next = dir.find(L'\\', pos);
        std::wstring seg = (next == std::wstring::npos) ? dir : dir.substr(0, next);
        if (!seg.empty()) {
            DWORD attr = GetFileAttributesW(seg.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(seg.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    if (err) *err = L"建目录失败 " + seg + L"（err=" + std::to_wstring(GetLastError()) + L"）";
                    return false;
                }
            } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                if (err) *err = L"路径段被同名文件占用: " + seg;
                return false;
            }
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return true;
}

// ---------- 外部进程统一执行器 ----------
// stdoutCapture 非空 → 管道捕获 stdout+stderr（上限 1MB，轮询+截止，防子进程挂死拖垮我们）；
// 为空 → 全部指向 NUL 设备（绝不继承工装自身的重定向管道，避免互写乱流）。
static bool RunProcessAndWait(const std::wstring& exePath, const std::wstring& argLine,
                              DWORD timeoutMs, std::string* stdoutCapture, DWORD* exitCode,
                              std::wstring* err)
{
    if (exitCode) *exitCode = 0;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = NULL, hWrite = NULL, hNul = NULL;
    std::wstring cmd = L"\"" + exePath + L"\"";
    if (!argLine.empty()) cmd += L" " + argLine;

    si.dwFlags = STARTF_USESTDHANDLES;
    if (stdoutCapture) {
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
            if (err) *err = L"建管道失败";
            return false;
        }
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = si.hStdError = hWrite;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    } else {
        hNul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        if (hNul == INVALID_HANDLE_VALUE) {
            if (err) *err = L"开 NUL 失败";
            return false;
        }
        si.hStdOutput = si.hStdError = si.hStdInput = hNul;
    }

    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');
    if (!CreateProcessW(exePath.c_str(), cmdbuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        if (err) *err = L"无法启动 " + exePath + L"（err=" + std::to_wstring(e) + L"）";
        if (hRead) CloseHandle(hRead);
        if (hWrite) CloseHandle(hWrite);
        if (hNul) CloseHandle(hNul);
        return false;
    }
    if (hWrite) { CloseHandle(hWrite); hWrite = NULL; } // 父侧写端必须关，否则 EOF 永不到

    bool timedOut = false;
    if (stdoutCapture) {
        stdoutCapture->clear();
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        char buf[4096];
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL)) break; // 全写端已关 → EOF
            if (avail > 0) {
                if (!ReadFile(hRead, buf, (DWORD)sizeof(buf), &got, NULL) || got == 0) break;
                stdoutCapture->append(buf, got);
                if (stdoutCapture->size() > (1u << 20)) break; // 捕获上限
                continue;
            }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break; // 已退出且管道排空
            if (GetTickCount64() > deadline) { timedOut = true; break; }
            Sleep(20);
        }
    }
    if (!timedOut && WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) timedOut = true;
    if (timedOut) {
        TerminateProcess(pi.hProcess, 0xFFFFFFFF);
        WaitForSingleObject(pi.hProcess, 5000); // #86：轮询等对象真正消失，不固定 sleep
    } else if (exitCode) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hRead) CloseHandle(hRead);
    if (hNul) CloseHandle(hNul);

    if (timedOut) {
        if (err) *err = L"进程超时（>" + std::to_wstring(timeoutMs / 1000) + L"s）已终止: " + exePath;
        return false;
    }
    return true;
}

static std::wstring SysDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetSystemDirectoryW(buf, ARRAYSIZE(buf));
    return buf;
}

// ---------- staging 路径校验（T10）----------
bool ComputeBinRootsFromModule(std::wstring* binRoot, std::wstring* workRoot)
{
    wchar_t buf[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(NULL, buf, (DWORD)ARRAYSIZE(buf)) == 0) return false;
    std::wstring p = buf;
    size_t s1 = p.find_last_of(L'\\'); // 去 exe 名 → <bin>\<子目录>
    if (s1 == std::wstring::npos) return false;
    p = p.substr(0, s1);
    size_t s2 = p.find_last_of(L'\\'); // 去子目录（Debug/Debug32/任意）→ <bin>
    if (s2 == std::wstring::npos || s2 == 0) return false;
    *binRoot = p.substr(0, s2);
    *workRoot = *binRoot + L"\\work";
    return true;
}

bool ValidateStagingPath(const std::wstring& stagingDir, const std::wstring& binWorkRoot,
                         SemVer* ver, std::wstring* err)
{
    std::wstring norm = NormalizeDir(stagingDir);
    if (norm.empty()) {
        if (err) *err = L"staging 路径无法解析";
        return false;
    }
    size_t sep = norm.find_last_of(L'\\');
    if (sep == std::wstring::npos || sep + 1 >= norm.size() || sep <= 2) {
        if (err) *err = L"staging 路径没有子目录名";
        return false;
    }
    std::wstring parent = norm.substr(0, sep);
    std::wstring name = norm.substr(sep + 1);
    std::wstring expect = NormalizeDir(binWorkRoot + L"\\staging");
    if (expect.empty() || _wcsicmp(parent.c_str(), expect.c_str()) != 0) {
        if (err) *err = L"staging 不在 bin\\work\\staging 之下（拒绝任意路径提权执行）";
        return false;
    }
    if (!ParseSemVer(name, ver)) {
        if (err) *err = L"staging 目录名不是合法版本号: " + name;
        return false;
    }
    DWORD attr = GetFileAttributesW(norm.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (err) *err = L"staging 目录不存在: " + norm;
        return false;
    }
    return true;
}

// ---------- 替换计划 ----------
static bool EnumTree(const std::wstring& root, const std::wstring& rel,
                     std::vector<std::wstring>* out, std::wstring* err)
{
    WIN32_FIND_DATAW fd;
    std::wstring pat = root + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true; // 空树合法（调用方查非空）
    bool ok = true;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        std::wstring childRel = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (err) *err = L"staging 树含符号链接/重解析点: " + childRel;
            ok = false;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!EnumTree(root + L"\\" + fd.cFileName, childRel, out, err)) { ok = false; break; }
        } else {
            out->push_back(childRel);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static bool HasRel(const std::vector<std::wstring>& v, const wchar_t* name)
{
    for (const auto& s : v)
        if (_wcsicmp(s.c_str(), name) == 0) return true;
    return false;
}

bool BuildReplacePlan(const std::wstring& stagingDir, const std::wstring& binRoot,
                      std::vector<FileOp>* plan, std::wstring* err)
{
    plan->clear();
    std::wstring x64Dir = stagingDir + L"\\x64";
    std::wstring x86Dir = stagingDir + L"\\x86";
    DWORD a64 = GetFileAttributesW(x64Dir.c_str());
    DWORD a86 = GetFileAttributesW(x86Dir.c_str());
    if (a64 == INVALID_FILE_ATTRIBUTES || !(a64 & FILE_ATTRIBUTE_DIRECTORY)) {
        if (err) *err = L"staging 缺 x64\\ 子树";
        return false;
    }
    if (a86 == INVALID_FILE_ATTRIBUTES || !(a86 & FILE_ATTRIBUTE_DIRECTORY)) {
        if (err) *err = L"staging 缺 x86\\ 子树（x64 与 x86 必须都换，缺一不可）";
        return false;
    }
    std::vector<std::wstring> f64, f86;
    if (!EnumTree(x64Dir, L"", &f64, err) || !EnumTree(x86Dir, L"", &f86, err)) return false;
    if (f64.empty()) {
        if (err) *err = L"64 位包内容为空";
        return false;
    }
    if (f86.empty()) {
        if (err) *err = L"32 位包内容为空（漏 32 位=修了但用户摸不到）";
        return false;
    }
    if (!HasRel(f64, L"ImeCore.dll")) {
        if (err) *err = L"x64 包缺 ImeCore.dll（TSF 重注册依赖）";
        return false;
    }
    if (!HasRel(f86, L"ImeCore.dll")) {
        if (err) *err = L"x86 包缺 ImeCore.dll（WOW6432Node 重注册依赖）";
        return false;
    }
    for (const auto& rel : f64)
        plan->push_back({rel, x64Dir + L"\\" + rel, binRoot + L"\\Debug\\" + rel});
    for (const auto& rel : f86)
        plan->push_back({rel, x86Dir + L"\\" + rel, binRoot + L"\\Debug32\\" + rel});
    return true;
}

// ---------- 落盘哈希自验 ----------
bool VerifyInstalledHashes(const std::vector<FileOp>& plan, std::wstring* err)
{
    for (const auto& op : plan) {
        std::string h1, h2;
        std::wstring e;
        if (!Sha256File(op.src, &h1, &e)) {
            if (err) *err = L"读包内哈希失败 " + op.rel + L": " + e;
            return false;
        }
        if (!Sha256File(op.dst, &h2, &e)) {
            if (err) *err = L"读落盘哈希失败 " + op.rel + L": " + e;
            return false;
        }
        if (h1 != h2) {
            if (err) *err = L"落盘与包内 SHA256 不一致: " + op.rel;
            return false;
        }
    }
    return true;
}

// ---------- 真实步骤实现 ----------
bool ReRegisterTsf(const std::wstring& dll64, const std::wstring& dll32, std::wstring* err)
{
    std::wstring sys = SysDir();
    std::wstring r64 = sys + L"\\regsvr32.exe"; // 提权进程是 x64 → System32
    std::wstring r32;
    {
        wchar_t wow[MAX_PATH] = {};
        GetWindowsDirectoryW(wow, ARRAYSIZE(wow));
        r32 = std::wstring(wow) + L"\\SysWOW64\\regsvr32.exe";
    }
    DWORD code = 0;
    if (!RunProcessAndWait(r64, L"/s \"" + dll64 + L"\"", 60000, NULL, &code, err)) return false;
    if (code != 0) {
        if (err) *err = L"x64 重注册失败（regsvr32 退出码 " + std::to_wstring(code) + L"）";
        return false;
    }
    if (!RunProcessAndWait(r32, L"/s \"" + dll32 + L"\"", 60000, NULL, &code, err)) return false;
    if (code != 0) {
        if (err) *err = L"x86 重注册失败（SysWOW64 regsvr32 退出码 " + std::to_wstring(code) + L"）";
        return false;
    }
    return true;
}

bool RunImecheck(const std::wstring& scriptPath, std::wstring* err)
{
    if (GetFileAttributesW(scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = L"imecheck 脚本不存在: " + scriptPath;
        return false;
    }
    std::wstring ps = SysDir() + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(ps.c_str()) == INVALID_FILE_ATTRIBUTES) ps = L"powershell.exe";
    DWORD code = 0;
    if (!RunProcessAndWait(ps, L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"",
                           120000, NULL, &code, err))
        return false;
    if (code != 0) {
        if (err) *err = L"imecheck 退出码 " + std::to_wstring(code) + L"（安装态检查未通过）";
        return false;
    }
    return true;
}

bool RunVersionProbe(const std::wstring& exePath, const std::wstring& versionStr,
                     std::wstring* err)
{
    if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = L"探针 exe 不存在: " + exePath;
        return false;
    }
    std::string out;
    DWORD code = 0;
    if (!RunProcessAndWait(exePath, L"--version", 20000, &out, &code, err)) return false;
    if (code != 0) {
        if (err) *err = L"探针退出码 " + std::to_wstring(code);
        return false;
    }
    if (out.empty()) {
        if (err) *err = L"探针无输出";
        return false;
    }
    // 双解码各搜一遍：EmitCliText 重定向分支写**无 BOM 的 UTF-16LE**（纯中文时
    // 无零字节、UTF-8 试解码也可能"成功"），只按一种编码解会假阴性。
    const unsigned char* b = (const unsigned char*)out.data();
    size_t n = out.size();
    size_t off = (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) ? 2 : 0;
    std::wstring w16;
    for (size_t i = off; i + 1 < n; i += 2)
        w16.push_back((wchar_t)((unsigned)b[i] | ((unsigned)b[i + 1] << 8)));
    std::wstring w8;
    int wn = MultiByteToWideChar(CP_UTF8, 0, out.data(), (int)out.size(), NULL, 0);
    if (wn > 0) {
        w8.resize((size_t)wn);
        MultiByteToWideChar(CP_UTF8, 0, out.data(), (int)out.size(), &w8[0], wn);
    }
    if (w16.find(versionStr) != std::wstring::npos || w8.find(versionStr) != std::wstring::npos)
        return true;
    if (err) *err = L"探针输出不含新版本号 " + versionStr + L"（磁盘件可能不是新版）";
    return false;
}

// ---------- 替换执行与回滚 ----------
namespace {
struct ReplaceState {
    std::wstring ts;
    std::vector<std::wstring> copied;                            // 已成功复制的 dst
    std::vector<std::pair<std::wstring, std::wstring>> renamed;  // {prev, 原 dst}
};

// 回滚：删新件 → .prev-* 原名改回。note 收集回滚自身的失败（必须上报，绝不吞）。
bool RollbackAll(ReplaceState* st, std::wstring* note)
{
    bool clean = true;
    for (auto it = st->copied.rbegin(); it != st->copied.rend(); ++it) {
        if (!DeleteFileW(it->c_str())) {
            clean = false;
            *note += L"删除新件失败 " + *it + L"(err=" + std::to_wstring(GetLastError()) + L")；";
        }
    }
    for (auto it = st->renamed.rbegin(); it != st->renamed.rend(); ++it) {
        if (!MoveFileExW(it->first.c_str(), it->second.c_str(), 0)) {
            clean = false;
            *note += L"旧件改回失败 " + it->second + L"(err=" + std::to_wstring(GetLastError()) + L")；";
        }
    }
    return clean;
}

// T9 两阶段：先全部 rename-aside（被锁 DLL 改名合法，#89），再全部复制。
// 任一失败 → 已做操作全部撤销，不留半替换态。
bool ApplyReplace(const std::vector<FileOp>& plan, ReplaceState* st, std::wstring* err)
{
    wchar_t tsbuf[32];
    _snwprintf_s(tsbuf, _TRUNCATE, L"%llu", (unsigned long long)time(NULL));
    st->ts = tsbuf;

    for (const auto& op : plan) {
        DWORD attr = GetFileAttributesW(op.dst.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        // 快照名 .prev-<ts>；同秒重跑撞名则加序号（幂等要求，绝不因旧快照存在而失败）。
        std::wstring prevBase = op.dst + L".prev-" + st->ts;
        std::wstring prev;
        bool moved = false;
        DWORD moveErr = 0;
        for (int seq = 1; seq <= 99; seq++) {
            prev = (seq == 1) ? prevBase : prevBase + L"-" + std::to_wstring(seq);
            if (MoveFileExW(op.dst.c_str(), prev.c_str(), 0)) { moved = true; break; }
            moveErr = GetLastError();
            if (moveErr != ERROR_ALREADY_EXISTS) break;
        }
        if (!moved) {
            *err = L"rename-aside 失败 " + op.rel + L"（err=" + std::to_wstring(moveErr) + L"）";
            std::wstring note;
            RollbackAll(st, &note); // copied 为空，只改回已挪的
            if (!note.empty()) *err += L"；回滚不完整: " + note;
            return false;
        }
        st->renamed.push_back({prev, op.dst});
    }
    for (const auto& op : plan) {
        if (!EnsureParentDirs(op.dst, err)) {
            std::wstring note;
            bool clean = RollbackAll(st, &note);
            *err += clean ? L"（已回滚）" : L"（回滚不完整: " + note + L"）";
            return false;
        }
        if (!CopyFileW(op.src.c_str(), op.dst.c_str(), FALSE)) {
            *err = L"复制新件失败 " + op.rel + L"（err=" + std::to_wstring(GetLastError()) + L"）";
            std::wstring note;
            bool clean = RollbackAll(st, &note);
            *err += clean ? L"（已回滚）" : L"（回滚不完整: " + note + L"）";
            return false;
        }
        st->copied.push_back(op.dst);
    }
    return true;
}
} // namespace

// ---------- 编排 ----------
bool ApplyUpdate(const ApplyRequest& req, const ApplyHooks& hooks, std::wstring* err,
                 std::wstring* log)
{
    std::vector<FileOp> plan;
    if (!BuildReplacePlan(req.stagingDir, req.binRoot, &plan, err)) return false;
    if (log) *log += L"替换计划 " + std::to_wstring(plan.size()) + L" 个文件（x64+x86 双位）\n";

    ReplaceState st;
    if (!ApplyReplace(plan, &st, err)) {
        if (log) *log += L"替换失败: " + *err + L"\n";
        return false;
    }
    if (log) *log += L"rename-aside + 复制完成（快照 .prev-" + st.ts + L"）\n";

    auto failWith = [&](const std::wstring& reason) -> bool {
        std::wstring note;
        bool clean = RollbackAll(&st, &note);
        *err = reason + (clean ? L"（已整体回滚，旧件原样恢复）"
                               : L"（回滚不完整: " + note + L"）");
        if (log) *log += *err + L"\n";
        return false;
    };

    std::wstring dll64 = req.binRoot + L"\\Debug\\ImeCore.dll";
    std::wstring dll32 = req.binRoot + L"\\Debug32\\ImeCore.dll";
    std::wstring sub;
    if (req.doRegister) {
        bool (*fn)(const std::wstring&, const std::wstring&, std::wstring*) =
            hooks.reRegister ? hooks.reRegister : ReRegisterTsf;
        if (!fn(dll64, dll32, &sub)) return failWith(L"TSF 重注册失败: " + sub);
        if (log) *log += L"TSF 重注册完成（x64 + WOW6432Node 两侧）\n";
    }

    // 自验（T12）：三项独立复核，任一不过 → 整体回滚。
    if (!req.imecheckScript.empty()) {
        bool (*fn)(const std::wstring&, std::wstring*) = hooks.imecheck ? hooks.imecheck : RunImecheck;
        if (!fn(req.imecheckScript, &sub)) return failWith(L"自验失败（imecheck）: " + sub);
        if (log) *log += L"自验 1/3 imecheck 通过\n";
    }
    {
        bool (*fn)(const std::wstring&, const std::wstring&, std::wstring*) =
            hooks.probe ? hooks.probe : RunVersionProbe;
        std::wstring probeExe = req.binRoot + L"\\Debug\\LingxiUI.exe";
        if (!fn(probeExe, req.versionStr, &sub)) return failWith(L"自验失败（探针 --version）: " + sub);
        if (log) *log += L"自验 2/3 探针版本匹配\n";
    }
    if (!VerifyInstalledHashes(plan, &sub)) return failWith(L"自验失败（落盘 SHA256）: " + sub);
    if (log) *log += L"自验 3/3 落盘哈希与包内一致\n";
    return true;
}

// ---------- 一次性 UAC 提权发起 ----------
bool LaunchElevatedApply(const std::wstring& selfExe, const std::wstring& stagingDir,
                         std::wstring* err)
{
    std::wstring args = L"--update-apply \"" + stagingDir + L"\"";
    HINSTANCE h = ShellExecuteW(NULL, L"runas", selfExe.c_str(), args.c_str(), NULL, SW_HIDE);
    if ((INT_PTR)h > 32) return true;
    if ((INT_PTR)h == (INT_PTR)1223 /*ERROR_CANCELLED*/) {
        if (err) *err = L"已取消 UAC 提权，未做任何改动";
        return false;
    }
    if (err) *err = L"无法启动提权进程（错误码 " + std::to_wstring((INT_PTR)h) + L"）";
    return false;
}

} // namespace updater
} // namespace lingxi
