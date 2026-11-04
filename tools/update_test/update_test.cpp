//
// ===== 自助更新单测工装（批次 2-4：检查 + 下载/校验/解压 + 替换/自验/回滚）=====
// 真断言（设计 §9）：版本比较 / JSON 解析 / 资产选择 / SUMS 解析 / 白名单 / 状态机 /
// SHA256 / zip-slip 与上限（测试内自建 stored zip 构造恶意包）/ tar 解包 / 解出复核 /
// staging 校验 / 替换计划 / rename-aside+复制+回滚全流程（假 bin 树，绝不碰真 bin）。
// 不打真实 GitHub；网络路径由批次 6 本地假服务集成测试覆盖。
//
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/update.h"
#include "../../common/update_fetch.h"
#include "../../common/update_apply.h"
#include "../../common/version.h"

using namespace lingxi::updater;

static int g_pass = 0, g_fail = 0;

// 控制台输出走 WriteFile（UTF-16LE）：wprintf 在 stdout 被重定向到管道/文件时
// 触发 CRT 无效参数断点（本工装实测 0x80000003），绕开 CRT 缓冲。
static void PrintW(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, (DWORD)(n * sizeof(wchar_t)), &written, nullptr);
}

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) { g_pass++; PrintW(L"[PASS] %ls\n", name); }               \
        else { g_fail++; PrintW(L"[FAIL] %ls\n", name); }                    \
    } while (0)

static bool SemEq(const SemVer& a, int M, int m, int p) {
    return a.major == M && a.minor == m && a.patch == p;
}

static HttpReply MakeReply(DWORD status, const std::string& body) {
    HttpReply r;
    r.status = status;
    r.body = body;
    return r;
}

// 完整合法 release JSON（含干扰字段：uploader.name 必须不被误取）。
static const char* kGoodJson =
    "{\"tag_name\":\"v2.5.1\",\"name\":\"Release 2.5.1\","
    "\"body\":\"\\u4fee\\u590d \\u2764 \\u53cc\\u62fc\","
    "\"target_commitish\":\"main\","
    "\"assets\":["
    "{\"name\":\"LingxiIme-win-x64.zip\",\"size\":111,\"browser_download_url\":\"https://github.com/o/r/releases/download/v2.5.1/LingxiIme-win-x64.zip\"},"
    "{\"name\":\"LingxiIme-win-x86.zip\",\"size\":222,\"browser_download_url\":\"https://github.com/o/r/releases/download/v2.5.1/LingxiIme-win-x86.zip\"},"
    "{\"name\":\"SHA256SUMS.txt\",\"size\":333,\"browser_download_url\":\"https://github.com/o/r/releases/download/v2.5.1/SHA256SUMS.txt\"},"
    "{\"name\":\"source.zip\",\"size\":999,\"browser_download_url\":\"https://github.com/o/r/archive/v2.5.1.zip\"}],"
    "\"uploader\":{\"name\":\"someone\",\"login\":\"someone\"}}";

static ReleaseInfo MakeRelease(const std::vector<std::wstring>& names) {
    ReleaseInfo rel;
    rel.tag = L"v9.9.9";
    for (const auto& n : names) {
        ReleaseAsset a;
        a.name = n;
        a.downloadUrl = L"https://github.com/o/r/releases/download/x/" + n;
        a.size = 10;
        rel.assets.push_back(a);
    }
    return rel;
}

// ---------- 批次 3 工装：stored zip 构造器 / 临时目录 ----------
static unsigned long Crc32Of(const std::string& d) {
    static unsigned long tab[256];
    static bool init = false;
    if (!init) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = true;
    }
    unsigned long c = 0xFFFFFFFFu;
    for (unsigned char b : d) c = tab[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct TEntry {
    std::string name;
    std::string data;
    unsigned short flags = 0;
    unsigned short method = 0;
    unsigned long crcOverride = 0xFFFFFFFFu;    // 哨兵=用真实值
    unsigned long uncompOverride = 0xFFFFFFFFu; // 哨兵=用真实值（zip64 测试用）
    unsigned long extAttr = 0;
    unsigned char madeByOs = 0; // 0=DOS 3=unix
    bool zip64Sentinel = false; // 强制写 0xFFFFFFFF 尺寸字段（zip64 拒收测试）
};

static void Put16(std::vector<unsigned char>& v, unsigned x) {
    v.push_back((unsigned char)(x & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
}
static void Put32(std::vector<unsigned char>& v, unsigned long x) {
    Put16(v, x & 0xFFFF);
    Put16(v, (unsigned)((x >> 16) & 0xFFFF));
}

static std::vector<unsigned char> BuildZip(const std::vector<TEntry>& es) {
    std::vector<unsigned char> z, cd;
    unsigned long off = 0;
    for (const auto& e : es) {
        unsigned long crc = e.crcOverride == 0xFFFFFFFFu ? Crc32Of(e.data) : e.crcOverride;
        unsigned long comp = (unsigned long)e.data.size();
        unsigned long uncomp =
            e.uncompOverride == 0xFFFFFFFFu ? (unsigned long)e.data.size() : e.uncompOverride;
        if (e.zip64Sentinel) uncomp = 0xFFFFFFFFu;
        Put32(z, 0x04034b50); Put16(z, 20); Put16(z, e.flags); Put16(z, e.method);
        Put16(z, 0); Put16(z, 0x2A21); // time=0 date=2001-01-01
        Put32(z, crc); Put32(z, comp); Put32(z, uncomp);
        Put16(z, (unsigned short)e.name.size()); Put16(z, 0);
        for (char c : e.name) z.push_back((unsigned char)c);
        for (char c : e.data) z.push_back((unsigned char)c);
        unsigned long locOff = off;
        off += 30 + (unsigned long)e.name.size() + (unsigned long)e.data.size();
        Put32(cd, 0x02014b50); Put16(cd, ((unsigned)e.madeByOs << 8) | 20); Put16(cd, 20);
        Put16(cd, e.flags); Put16(cd, e.method); Put16(cd, 0); Put16(cd, 0x2A21);
        Put32(cd, crc); Put32(cd, comp); Put32(cd, uncomp);
        Put16(cd, (unsigned short)e.name.size()); Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0); Put32(cd, e.extAttr); Put32(cd, locOff);
        for (char c : e.name) cd.push_back((unsigned char)c);
    }
    unsigned long cdOff = off, cdSize = (unsigned long)cd.size();
    for (unsigned char b : cd) z.push_back(b);
    Put32(z, 0x06054b50); Put16(z, 0); Put16(z, 0);
    Put16(z, (unsigned short)es.size()); Put16(z, (unsigned short)es.size());
    Put32(z, cdSize); Put32(z, cdOff); Put16(z, 0);
    return z;
}

static std::wstring g_tmp; // %TEMP%\lingxi-updatetest-<pid>
static bool MkdirP(const std::wstring& path) {
    // 跳过盘符根（"C:\" 必存在，CreateDirectoryW("C:") 会报错）。
    size_t i = (path.size() >= 2 && path[1] == L':') ? 3 : 0;
    while (i <= path.size()) {
        size_t sep = path.find(L'\\', i);
        if (sep == std::wstring::npos) sep = path.size();
        if (sep > i) { // 非空段才建（自顶向下，PATH_NOT_FOUND 不可能）
            std::wstring acc = path.substr(0, sep);
            if (!CreateDirectoryW(acc.c_str(), nullptr)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    PrintW(L"  MkdirP 失败段=%ls err=%lu\n", acc.c_str(), e);
                    return false;
                }
            }
        }
        i = sep + 1;
    }
    return true;
}
static std::wstring TPath(const std::wstring& name) { return g_tmp + L"\\" + name; }
static bool WriteStr(const std::wstring& path, const std::string& s) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = TRUE;
    size_t w = 0;
    while (w < s.size()) {
        DWORD n = 0;
        DWORD want = (DWORD)(s.size() - w > 65536 ? 65536 : s.size() - w);
        ok = WriteFile(h, s.data() + w, want, &n, nullptr) && n == want;
        if (!ok) break;
        w += n;
    }
    CloseHandle(h);
    return ok != FALSE;
}
static bool WriteZip(const std::vector<unsigned char>& z, const std::wstring& name) {
    return WriteStr(TPath(name), std::string((const char*)z.data(), z.size()));
}
static std::string ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string s;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) s.append(buf, n);
    CloseHandle(h);
    return s;
}
// 写临时包 → 预校验。wantErrSub 非空时还要求错误文案含该子串。
static bool PrefZip(const std::vector<unsigned char>& z, const std::wstring& pkg = L"pkg.zip",
                    std::vector<lingxi::updater::ZipEntry>* out = nullptr,
                    std::wstring* errOut = nullptr) {
    if (!WriteZip(z, pkg)) return false;
    std::wstring err;
    std::vector<lingxi::updater::ZipEntry> entries;
    bool ok = lingxi::updater::PreflightZip(TPath(pkg), &entries, &err);
    if (out) *out = std::move(entries);
    if (errOut) *errOut = err;
    return ok;
}

// ---------- 批次 4：hooks 桩（记录调用；真实文件替换/哈希自验不打桩）----------
static int g_regCalls = 0;
static bool g_regOk = true;
static std::wstring g_regDll64, g_regDll32;
static bool StubReRegister(const std::wstring& d64, const std::wstring& d32, std::wstring*) {
    g_regCalls++;
    g_regDll64 = d64;
    g_regDll32 = d32;
    return g_regOk;
}
static int g_imeCalls = 0;
static bool g_imeOk = true;
static bool StubImecheck(const std::wstring&, std::wstring*) { g_imeCalls++; return g_imeOk; }

// T2 注入桩：imecheck 时机（复制完成、自验阶段）把新 dst 独占锁死——随后回滚
// 删新件必败 = 回滚不完整场景。返回 false 触发 failWith 回滚链。
static HANDLE g_t2Lock = NULL;
static std::wstring g_t2LockPath;
static bool StubImecheckLockDst(const std::wstring&, std::wstring*) {
    if (g_t2Lock) { CloseHandle(g_t2Lock); g_t2Lock = NULL; }
    // GENERIC_READ + 仅共享读：不给 DELETE 即"宿主加载态"（删必败、改名让位败），
    // 同时自断言可读回内容（GENERIC_WRITE 会挡住工装的 ReadAll 共享读）
    g_t2Lock = CreateFileW(g_t2LockPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    return false;
}

// 数 dst 旁边 .prev-* 快照个数（回滚断言用）。
static int CountPrev(const std::wstring& dstPath) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dstPath + L".prev-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
// 最早建出的 .prev-*（FindFirst 按名字序；时间戳定宽 10 位 → 字典序=时间序）。
static std::string OldestPrevContent(const std::wstring& dstPath) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dstPath + L".prev-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::wstring dir = dstPath.substr(0, dstPath.find_last_of(L'\\'));
    FindClose(h);
    return ReadAll(dir + L"\\" + fd.cFileName);
}

// ---------- 批次 6：--e2e 端到端演练（本地假服务全链路，只动假 bin 树） ----------
static long long g_e2eDone = 0;
static bool E2eProgress(long long done, long long, void*) { g_e2eDone = done; return true; }

static int RunE2E() {
    PrintW(L"== update_test --e2e: 本地假服务端到端演练（批次 6）==\n");
    wchar_t base[260] = {};
    GetTempPathW(ARRAYSIZE(base), base);
    g_tmp = std::wstring(base) + L"lingxi-e2e-" + std::to_wstring(GetCurrentProcessId());
    {
        wchar_t tb[256] = {};
        DWORD n = GetEnvironmentVariableW(L"LINGXI_UPDATE_TEST_BASE", tb, ARRAYSIZE(tb));
        CHECK(n > 0, L"演练前置：LINGXI_UPDATE_TEST_BASE 已设（假服务须已就绪）");
        if (n == 0) { PrintW(L"== E2E 汇总：PASS=%d FAIL=%d ==\n", g_pass, g_fail); return 1; }
    }
    const std::wstring dl = g_tmp + L"\\dl";
    CHECK(MkdirP(dl), L"建 dl 目录");

    // ----- 状态机四类服务端形态（真 HTTP，走演练钩子） -----
    ReleaseInfo relNew;
    SemVer remote = {};
    std::wstring err;
    State st = CheckLatest(L"lingxi/new", nullptr, &relNew, &remote, &err);
    PrintW(L"[diag] first CheckLatest state=%d err='%ls'\n", (int)st, err.c_str());
    CHECK(st == State::NewVersion && SemEq(remote, 9, 9, 9) && !relNew.notes.empty() &&
              relNew.assets.size() == 3,
          L"假新版 v9.9.9 → NewVersion（附 notes 与三资产）");
    {
        ReleaseInfo r; SemVer rv; std::wstring e2;
        CHECK(CheckLatest(L"lingxi/same", nullptr, &r, &rv, &e2) == State::UpToDate,
              L"tag=本机版本 → 已是最新");
        e2.clear(); // 工装缺陷修正：不得带上一条用例的残留 err 进断言
        State se = CheckLatest(L"lingxi/empty", nullptr, &r, &rv, &e2);
        CHECK(se == State::NotPublished && e2.empty(),
              L"404 → 尚未发布任何版本（正常态，无错误文案）");
        CHECK(CheckLatest(L"lingxi/missing86", nullptr, &r, &rv, &e2) == State::Incomplete,
              L"缺 x86 资产 → 发布包不完整（不可用）");
    }

    // ----- 全链路 happy path：检查→选资产→下载→校验→解压→staging→替换→自验 -----
    SelectedAssets sel;
    std::wstring reason;
    CHECK(SelectAssets(relNew, &sel, &reason), L"契约三资产选择通过");
    std::wstring sumsPath = dl + L"\\SHA256SUMS.txt";
    CHECK(DownloadToFile(sel.sums.downloadUrl, sumsPath, 64u * 1024u, nullptr, nullptr, nullptr, &err),
          L"SHA256SUMS.txt 下载（假服务 http 回环钩子放行）");
    std::vector<std::pair<std::wstring, std::string>> sums;
    CHECK(ParseSha256Sums(ReadAll(sumsPath), &sums, &err) && sums.size() == 2, L"SUMS 解析 2 条");
    std::wstring z64 = dl + L"\\LingxiIme-win-x64.zip", z86 = dl + L"\\LingxiIme-win-x86.zip";
    g_e2eDone = 0;
    CHECK(DownloadToFile(sel.x64.downloadUrl, z64, kMaxDownloadBytes, nullptr, E2eProgress, nullptr, &err) &&
              g_e2eDone > 0,
          L"x64 包下载 + 进度回调有字节数");
    CHECK(DownloadToFile(sel.x86.downloadUrl, z86, kMaxDownloadBytes, nullptr, nullptr, nullptr, &err),
          L"x86 包下载");
    CHECK(VerifyAgainstSums(sums, L"LingxiIme-win-x64.zip", z64, &err) &&
              VerifyAgainstSums(sums, L"LingxiIme-win-x86.zip", z86, &err),
          L"两包 SHA256 与 SUMS 匹配");
    std::vector<ZipEntry> ents;
    CHECK(PreflightZip(z64, &ents, &err) && PreflightZip(z86, &ents, &err), L"两包 zip 预校验通过");

    const std::wstring fakeBin = g_tmp + L"\\bin";
    const std::wstring workRoot = fakeBin + L"\\work";
    const std::wstring staging = workRoot + L"\\staging\\9.9.9";
    CHECK(MkdirP(staging + L"\\x64") && MkdirP(staging + L"\\x86"), L"建 staging 双树");
    CHECK(ExtractZipTo(z64, staging + L"\\x64", nullptr, &err) &&
              ExtractZipTo(z86, staging + L"\\x86", nullptr, &err),
          L"两包解压到 staging（tar.exe 真实路径）");
    CHECK(VerifyExtractedDir(staging + L"\\x64", &err) && VerifyExtractedDir(staging + L"\\x86", &err),
          L"解出后复核通过");
    {
        SemVer sv;
        CHECK(ValidateStagingPath(staging, workRoot, &sv, &err) && SemEq(sv, 9, 9, 9),
              L"ValidateStagingPath 自检（提权侧同口径）");
    }
    CHECK(MkdirP(fakeBin + L"\\Debug") && MkdirP(fakeBin + L"\\Debug32") &&
              WriteStr(fakeBin + L"\\Debug\\ImeCore.dll", "OLD64") &&
              WriteStr(fakeBin + L"\\Debug\\LingxiUI.exe", "OLDEXE") &&
              WriteStr(fakeBin + L"\\Debug32\\ImeCore.dll", "OLD32"),
          L"假 bin 树放旧件（真 bin 零接触）");
    const std::wstring imePs = g_tmp + L"\\imecheck_fake.ps1";
    CHECK(WriteStr(imePs, "exit 0\r\n"), L"写假 imecheck.ps1（真 RunImecheck 走 powershell 退出码）");
    ApplyRequest req;
    req.binRoot = fakeBin;
    req.stagingDir = staging;
    req.versionStr = L"9.9.9";
    req.imecheckScript = imePs;
    req.doRegister = true;
    ApplyHooks hooks;
    hooks.reRegister = StubReRegister; // 真实 regsvr32 需管理员+真 DLL，演练打桩
    std::wstring log;
    g_regCalls = 0;
    bool ok = ApplyUpdate(req, hooks, &err, &log);
    CHECK(ok, L"ApplyUpdate 全流程成功（真探针自验：staging 内 LingxiUI.exe=本 exe 副本输出 9.9.9）");
    CHECK(ok && ReadAll(fakeBin + L"\\Debug\\ImeCore.dll") == "E2E-NEW64" &&
              ReadAll(fakeBin + L"\\Debug32\\ImeCore.dll") == "E2E-NEW32" &&
              ReadAll(fakeBin + L"\\Debug\\data\\pinyin.txt") == "PY64",
          L"x64 与 x86 两份都换成新件 + 子目录文件落位");
    CHECK(g_regCalls == 1 && g_regDll64 == fakeBin + L"\\Debug\\ImeCore.dll" &&
              g_regDll32 == fakeBin + L"\\Debug32\\ImeCore.dll",
          L"重注册桩收到两侧 DLL 路径（真实注册留真机验证）");
    CHECK(CountPrev(fakeBin + L"\\Debug\\ImeCore.dll") >= 1 &&
              OldestPrevContent(fakeBin + L"\\Debug\\ImeCore.dll") == "OLD64",
          L".prev 快照保留且内容为旧件（回滚依据在位）");
    {
        std::wstring err2, log2;
        CHECK(ApplyUpdate(req, hooks, &err2, &log2), L"幂等重跑：第二次 ApplyUpdate 仍成功");
    }

    // ----- 篡改包：下载成功但哈希不匹配 → 拒，绝不进 staging -----
    {
        ReleaseInfo relBad;
        SemVer rv;
        std::wstring e2;
        State sb = CheckLatest(L"lingxi/badsum", nullptr, &relBad, &rv, &e2);
        SelectedAssets selB;
        std::wstring why;
        CHECK(sb == State::NewVersion && SelectAssets(relBad, &selB, &why), L"篡改发布：检查与选资产通过");
        std::wstring badZip = dl + L"\\bad-x64.zip";
        CHECK(DownloadToFile(selB.x64.downloadUrl, badZip, kMaxDownloadBytes, nullptr, nullptr, nullptr, &e2),
              L"篡改包下载本身成功（200）");
        CHECK(!VerifyAgainstSums(sums, L"LingxiIme-win-x64.zip", badZip, &e2) && !e2.empty(),
              L"篡改包 SHA256 不匹配 → 拒（整包丢弃，绝不替换）");
    }
    PrintW(L"== E2E 汇总：PASS=%d FAIL=%d ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    // 集成测试把本 exe 复制为假 bin 树的 LingxiUI.exe 充当探针桩：
    // 按 EmitCliText 重定向分支形态输出无 BOM UTF-16LE（真实探针解码路径）。
    if (argc >= 2 && wcscmp(argv[1], L"--version") == 0) {
        const wchar_t* msg = L"灵犀输入法 9.9.9（构建于 fake probe）\n";
        DWORD w = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg,
                  (DWORD)(wcslen(msg) * sizeof(wchar_t)), &w, nullptr);
        return 0;
    }
    if (argc >= 2 && wcscmp(argv[1], L"--e2e") == 0)
        return RunE2E();
    PrintW(L"== update_test: 检查 + 下载/校验/解压 + 替换/自验/回滚（批次 2-4）==\n");

    // ---------- 版本真源一致性 ----------
    {
        SemVer v;
        bool ok = ParseSemVer(LINGXI_VERSION_WSTR, &v);
        CHECK(ok && SemEq(v, LINGXI_VERSION_MAJOR, LINGXI_VERSION_MINOR, LINGXI_VERSION_PATCH),
              L"version.h 真源自洽：STRING 与三段数字一致");
        SemVer loc = LocalVersion();
        CHECK(SemEq(loc, LINGXI_VERSION_MAJOR, LINGXI_VERSION_MINOR, LINGXI_VERSION_PATCH),
              L"LocalVersion 取自真源");
    }

    // ---------- CheckLatest 入参校验（批次 6 缺陷回归锁定：字符集曾漏 '/'） ----------
    {
        volatile LONG cancel = 1;
        std::wstring e;
        State s = CheckLatest(L"lingxi/new", &cancel, nullptr, nullptr, &e);
        CHECK(s == State::Failed && e == L"已取消",
              L"合法 owner/repo 过校验抵达 HTTP 层（取消口）——'/' 误判非法的回归锁定");
        CHECK(CheckLatest(L"a/b/c", nullptr, nullptr, nullptr, &e) == State::Failed &&
                  e.find(L"owner/repo") != std::wstring::npos,
              L"双斜杠 → 格式拒");
        CHECK(CheckLatest(L"a b/c", nullptr, nullptr, nullptr, &e) == State::Failed &&
                  e.find(L"非法字符") != std::wstring::npos,
              L"空格 → 非法字符拒（收紧面未被 '/' 放宽破坏）");
    }

    // ---------- ParseSemVer ----------
    {
        SemVer v;
        CHECK(ParseSemVer(L"1.0", &v) && SemEq(v, 1, 0, 0), L"1.0 解析为 1.0.0（兼容旧串）");
        CHECK(ParseSemVer(L"v1.2.3", &v) && SemEq(v, 1, 2, 3), L"v 前缀剥离");
        CHECK(ParseSemVer(L"V2.0.0", &v) && SemEq(v, 2, 0, 0), L"V 大写前缀");
        CHECK(ParseSemVer(L" 1.2.3 ", &v) && SemEq(v, 1, 2, 3), L"首尾空白容忍");
        CHECK(ParseSemVer(L"1.2.3-rc1", &v) == false, L"预发布后缀拒绝（宁可不更不误更）");
        CHECK(ParseSemVer(L"1.2.3+build9", &v) == false, L"构建元数据后缀拒绝");
        CHECK(ParseSemVer(L"1.2.3.4", &v) == false, L"4 段拒绝");
        CHECK(ParseSemVer(L"1..2", &v) == false, L"空段拒绝");
        CHECK(ParseSemVer(L"-1.2", &v) == false, L"负号拒绝");
        CHECK(ParseSemVer(L"abc", &v) == false, L"非数字拒绝");
        CHECK(ParseSemVer(L"", &v) == false, L"空串拒绝");
        CHECK(ParseSemVer(L"v", &v) == false, L"裸 v 拒绝");
        CHECK(ParseSemVer(L"01.2", &v) == false, L"前导零拒绝");
        CHECK(ParseSemVer(L"99999999.1.1", &v) == false, L"超长段拒绝（防溢出）");
        CHECK(ParseSemVer(L"9999999.1.1", &v) && SemEq(v, 9999999, 1, 1), L"7 位段边界放行");
    }

    // ---------- CompareSemVer：数值比较而非字符串 ----------
    {
        SemVer a, b;
        ParseSemVer(L"1.10.0", &a);
        ParseSemVer(L"1.9.0", &b);
        CHECK(CompareSemVer(a, b) == 1, L"1.10.0 > 1.9.0（字符串比较会反，钉死数值序）");
        ParseSemVer(L"1.0", &a);
        ParseSemVer(L"1.0.0", &b);
        CHECK(CompareSemVer(a, b) == 0, L"1.0 == 1.0.0");
        ParseSemVer(L"2.0.0", &a);
        ParseSemVer(L"1.99.99", &b);
        CHECK(CompareSemVer(a, b) == 1, L"2.0.0 > 1.99.99");
        ParseSemVer(L"1.0.1", &a);
        CHECK(CompareSemVer(b, a) == 1, L"1.99.99 > 1.0.1（minor 定序）");
    }

    // ---------- ParseReleaseJson ----------
    {
        ReleaseInfo rel;
        std::wstring err;
        bool ok = ParseReleaseJson(kGoodJson, &rel, &err);
        CHECK(ok, L"合法 JSON 解析成功");
        CHECK(rel.tag == L"v2.5.1", L"tag_name 提取");
        CHECK(rel.notes.find(L"修复") != std::wstring::npos &&
                  rel.notes.find(L'\u2764') != std::wstring::npos,
              L"body \\u 转义（中文+代理外 BMP）还原");
        CHECK(rel.assets.size() == 4, L"assets 全量提取");
        CHECK(rel.assets.size() == 4 && rel.assets[0].size == 111, L"asset size 数值提取");
        CHECK(rel.assets.size() == 4 && !rel.assets[0].downloadUrl.empty(),
              L"browser_download_url 提取");
        // 干扰字段：uploader.name 不得被当成资产名。
        bool uploaderLeak = false;
        for (const auto& a : rel.assets)
            if (a.name == L"someone") uploaderLeak = true;
        CHECK(!uploaderLeak, L"嵌套对象字段不污染资产列表（结构感知）");

        ReleaseInfo bad;
        CHECK(!ParseReleaseJson("{not json", &bad, &err), L"非法 JSON 拒绝");
        CHECK(!ParseReleaseJson("{\"name\":\"x\"}", &bad, &err), L"缺 tag_name 拒绝");
        CHECK(!ParseReleaseJson("{\"tag_name\":\"v1\"}", &bad, &err), L"缺 assets 拒绝");
        std::string deep = std::string(60, '[') + std::string(60, ']');
        CHECK(!ParseReleaseJson(deep, &bad, &err), L"超深递归拒绝（防 DoS）");
    }

    // ---------- SelectAssets（契约）----------
    {
        SelectedAssets sel;
        std::wstring reason;
        ReleaseInfo full = MakeRelease(
            {L"LingxiIme-win-x64.zip", L"LingxiIme-win-x86.zip", L"SHA256SUMS.txt", L"extra.bin"});
        CHECK(SelectAssets(full, &sel, &reason), L"三资产齐备（多余资产忽略）");
        CHECK(sel.x64.name == L"LingxiIme-win-x64.zip" && sel.x86.name == L"LingxiIme-win-x86.zip" &&
                  sel.sums.name == L"SHA256SUMS.txt",
              L"选择结果与契约名对应");

        ReleaseInfo noX86 = MakeRelease({L"LingxiIme-win-x64.zip", L"SHA256SUMS.txt"});
        CHECK(!SelectAssets(noX86, &sel, &reason) && reason.find(L"x86") != std::wstring::npos,
              L"缺 x86 → 不可用（历史教训：漏 32 位=修了摸不到）");
        ReleaseInfo noSums = MakeRelease({L"LingxiIme-win-x64.zip", L"LingxiIme-win-x86.zip"});
        CHECK(!SelectAssets(noSums, &sel, &reason) && reason.find(L"SHA256SUMS") != std::wstring::npos,
              L"缺 SHA256SUMS → 不可用（绝不降级直装）");
        ReleaseInfo wrongName = MakeRelease(
            {L"LingxiIme-win-x64.ZIP", L"LingxiIme-win-x86.zip", L"SHA256SUMS.txt"});
        CHECK(!SelectAssets(wrongName, &sel, &reason), L"资产名大小写不符 → 不可用（逐字符契约）");
    }

    // ---------- ParseSha256Sums ----------
    {
        std::vector<std::pair<std::wstring, std::string>> sums;
        std::wstring err;
        const std::string good =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  LingxiIme-win-x64.zip\n"
            "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789 *LingxiIme-win-x86.zip\n";
        CHECK(ParseSha256Sums(good, &sums, &err) && sums.size() == 2, L"SUMS 合法两行（双空格/binary 标记）");
        CHECK(sums.size() == 2 && sums[0].second == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
              L"hex 原样小写保留");
        CHECK(sums.size() == 2 && sums[1].second.size() == 64 && sums[1].second[0] == 'a',
              L"大写 hex 归一为小写");
        CHECK(sums.size() == 2 && sums[1].first == L"LingxiIme-win-x86.zip", L"binary 标记 * 剥离");
        CHECK(!ParseSha256Sums("deadbeef  short.txt\n", &sums, &err), L"hex 长度不足 64 → 整文件拒绝");
        CHECK(!ParseSha256Sums("0123456789abcdeg0123456789abcdef0123456789abcdef0123456789abcdef  x.zip\n",
                               &sums, &err),
              L"非法 hex 字符拒绝");
        CHECK(!ParseSha256Sums("", &sums, &err), L"空 SUMS 拒绝");
    }

    // ---------- IsUrlAllowed（T2）----------
    {
        CHECK(IsUrlAllowed(L"https://api.github.com/repos/o/r/releases/latest"), L"api.github.com https 放行");
        CHECK(IsUrlAllowed(L"https://github.com/o/r/releases/download/v1/a.zip"), L"github.com https 放行");
        CHECK(IsUrlAllowed(L"https://objects.githubusercontent.com/x/y/z"), L"objects.githubusercontent.com 放行");
        CHECK(!IsUrlAllowed(L"http://api.github.com/x"), L"http 明文拒绝");
        CHECK(!IsUrlAllowed(L"https://api.github.com.evil.com/x"), L"子域伪装拒绝");
        CHECK(!IsUrlAllowed(L"https://evil.com/api.github.com"), L"路径伪装域名拒绝");
        CHECK(!IsUrlAllowed(L"https://github.com:8443@evil.com/"), L"userinfo 夹带拒绝");
        CHECK(!IsUrlAllowed(L"garbage"), L"非法 URL 拒绝");
    }

    // ---------- 状态机（ProcessLatestReply，不打网）----------
    {
        SemVer local = LocalVersion(); // 1.0.0
        ReleaseInfo rel;
        SemVer remote;
        std::wstring err;

        CHECK(ProcessLatestReply(MakeReply(404, "{}"), local, &rel, &remote, &err) == State::NotPublished,
              L"404 → 尚未发布（正常态：无弹窗无错误）");
        CHECK(StateText(State::NotPublished) == std::wstring(L"尚未发布任何版本"),
              L"未发布文案钉死");

        HttpReply netfail;
        netfail.status = 0;
        netfail.err = L"连接失败（DNS/网络）";
        CHECK(ProcessLatestReply(netfail, local, &rel, &remote, &err) == State::Failed &&
                  err.find(L"网络") != std::wstring::npos,
              L"网络层失败 → Failed 带原因");

        CHECK(ProcessLatestReply(MakeReply(403, ""), local, &rel, &remote, &err) == State::Failed &&
                  err.find(L"限流") != std::wstring::npos,
              L"403 → 限流文案（不静默）");
        CHECK(ProcessLatestReply(MakeReply(429, ""), local, &rel, &remote, &err) == State::Failed,
              L"429 → 限流");

        CHECK(ProcessLatestReply(MakeReply(200, "{oops"), local, &rel, &remote, &err) == State::Failed,
              L"200+非法 JSON → Failed");
        ReleaseInfo noSumsRel = MakeRelease({L"LingxiIme-win-x64.zip", L"LingxiIme-win-x86.zip"});
        std::string noSumsJson =
            "{\"tag_name\":\"v9.9.9\",\"assets\":["
            "{\"name\":\"LingxiIme-win-x64.zip\",\"browser_download_url\":\"https://github.com/o/r/x64.zip\"},"
            "{\"name\":\"LingxiIme-win-x86.zip\",\"browser_download_url\":\"https://github.com/o/r/x86.zip\"}]}";
        State st = ProcessLatestReply(MakeReply(200, noSumsJson), local, &rel, &remote, &err);
        CHECK(st == State::Incomplete, L"缺 SUMS → Incomplete（不可用态，非 Failed 非直装）");

        st = ProcessLatestReply(MakeReply(200, kGoodJson), local, &rel, &remote, &err);
        CHECK(st == State::NewVersion && SemEq(remote, 2, 5, 1), L"v2.5.1 > 1.0.0 → 发现新版");
        CHECK(rel.notes.find(L"双拼") != std::wstring::npos, L"notes 随状态透出（UI 展示更新说明）");

        std::string sameJson = "{\"tag_name\":\"v1.0.0\",\"assets\":["
                               "{\"name\":\"LingxiIme-win-x64.zip\",\"browser_download_url\":\"https://github.com/o/r/a\"},"
                               "{\"name\":\"LingxiIme-win-x86.zip\",\"browser_download_url\":\"https://github.com/o/r/b\"},"
                               "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://github.com/o/r/c\"}]}";
        CHECK(ProcessLatestReply(MakeReply(200, sameJson), local, &rel, &remote, &err) == State::UpToDate,
              L"同版本 → 已是最新");
        std::string oldJson = "{\"tag_name\":\"v0.9.0\",\"assets\":["
                              "{\"name\":\"LingxiIme-win-x64.zip\",\"browser_download_url\":\"https://github.com/o/r/a\"},"
                              "{\"name\":\"LingxiIme-win-x86.zip\",\"browser_download_url\":\"https://github.com/o/r/b\"},"
                              "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://github.com/o/r/c\"}]}";
        CHECK(ProcessLatestReply(MakeReply(200, oldJson), local, &rel, &remote, &err) == State::UpToDate,
              L"远端更旧 → 已是最新（不降级安装）");
        std::string badTag = "{\"tag_name\":\"nightly-2026\",\"assets\":["
                             "{\"name\":\"LingxiIme-win-x64.zip\",\"browser_download_url\":\"https://github.com/o/r/a\"},"
                             "{\"name\":\"LingxiIme-win-x86.zip\",\"browser_download_url\":\"https://github.com/o/r/b\"},"
                             "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://github.com/o/r/c\"}]}";
        CHECK(ProcessLatestReply(MakeReply(200, badTag), local, &rel, &remote, &err) == State::Failed,
              L"tag 非法版本串 → Failed（不猜版本）");
    }

    // ---------- CheckLatest 本地前置分支（不出网）----------
    {
        ReleaseInfo rel;
        SemVer remote;
        std::wstring err;
        CHECK(CheckLatest(L"", nullptr, &rel, &remote, &err) == State::NotConfigured,
              L"repo 留空 → 未配置更新源（正常态）");
        CHECK(CheckLatest(L"justname", nullptr, &rel, &remote, &err) == State::Failed,
              L"缺 owner 段 → Failed");
        CHECK(CheckLatest(L"a/b/c", nullptr, &rel, &remote, &err) == State::Failed,
              L"多余斜杠段 → Failed");
        CHECK(CheckLatest(L"../evil/x", nullptr, &rel, &remote, &err) == State::Failed,
              L"路径穿越字符 → Failed（URL 注入面收口）");
        CHECK(CheckLatest(L"o/r%20x", nullptr, &rel, &remote, &err) == State::Failed,
              L"百分号编码夹带 → Failed");
    }

    // ---------- 批次 3：临时目录 ----------
    {
        wchar_t base[MAX_PATH] = {};
        GetTempPathW(ARRAYSIZE(base), base);
        g_tmp = std::wstring(base) + L"lingxi-updatetest-" + std::to_wstring(GetCurrentProcessId());
        CHECK(MkdirP(g_tmp), L"临时测试目录就绪（%TEMP%，不碰 bin）");
    }

    // ---------- 批次 3：Sha256File / VerifyAgainstSums ----------
    {
        std::string hex;
        std::wstring err;
        CHECK(WriteStr(TPath(L"sha_abc.bin"), "abc"), L"写 sha_abc.bin");
        CHECK(Sha256File(TPath(L"sha_abc.bin"), &hex, &err) &&
                  hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              L"SHA256('abc') 已知向量");
        CHECK(WriteStr(TPath(L"sha_empty.bin"), ""), L"写 sha_empty.bin");
        CHECK(Sha256File(TPath(L"sha_empty.bin"), &hex, &err) &&
                  hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              L"SHA256(空文件) 已知向量");
        CHECK(!Sha256File(TPath(L"definitely_missing.bin"), &hex, &err), L"缺文件 → false");
        CHECK(WriteStr(TPath(L"v.bin"), "abc"), L"写 v.bin");
        std::vector<std::pair<std::wstring, std::string>> sums;
        sums.push_back({L"v.bin",
                        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
        CHECK(VerifyAgainstSums(sums, L"v.bin", TPath(L"v.bin"), &err), L"SUMS 匹配 → 通过");
        WriteStr(TPath(L"v.bin"), "abd"); // 篡改
        CHECK(!VerifyAgainstSums(sums, L"v.bin", TPath(L"v.bin"), &err) &&
                  err.find(L"校验失败") != std::wstring::npos,
              L"篡改 → 校验失败（整包丢弃语义）");
        CHECK(!VerifyAgainstSums(sums, L"w.bin", TPath(L"v.bin"), &err) &&
                  err.find(L"缺少") != std::wstring::npos,
              L"SUMS 缺行 → 拒绝（不降级）");
    }

    // ---------- 批次 3：PreflightZip 合法路径 ----------
    {
        std::vector<ZipEntry> out;
        std::wstring err;
        auto good = BuildZip({{"ImeCore.dll", "dll-bytes"}, {"data/pinyin.txt", "py-data"}});
        CHECK(PrefZip(good, L"pkg.zip", &out, &err) && out.size() == 2,
              L"合法包（含 data/ 子目录）通过");
        auto unixReg = BuildZip({{"ImeCore.dll", "x", 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x81ED0000, 3}});
        CHECK(PrefZip(unixReg), L"unix 普通文件条目（0x8000）放行");
        auto withDir = BuildZip({{"data/", ""}, {"data/a.txt", "z"}});
        CHECK(PrefZip(withDir), L"目录条目放行");
    }

    // ---------- 批次 3：PreflightZip 恶意/超限全拒（T4/T5/T6）----------
    {
        std::wstring err;
        struct Bad { std::vector<TEntry> es; const wchar_t* name; const wchar_t* errSub; };
        std::vector<TEntry> long1;
        long1.push_back({std::string(65, 'x'), "d"});
        std::vector<TEntry> many;
        for (int i = 0; i < 2049; i++) many.push_back({"f" + std::to_string(i) + ".bin", "x"});
        std::vector<TEntry> bomb;
        for (int i = 0; i < 5; i++)
            bomb.push_back({"b" + std::to_string(i) + ".bin", "x", 0, 0, 0xFFFFFFFFu, 60u << 20});
        std::vector<TEntry> dup = {{"A.txt", "1"}, {"a.txt", "2"}};
        std::vector<Bad> bads = {
            {{{"../evil.txt", "x"}}, L"zip-slip .. 段拒", L".."},
            {{{"/abs.txt", "x"}}, L"绝对路径拒", L"空路径段"},
            {{{"C:evil.txt", "x"}}, L"盘符冒号拒", L"非法字符"},
            {{{"foo\\bar.txt", "x"}}, L"反斜杠拒", L"非法字符"},
            {{{"CON.txt", "x"}}, L"保留名 CON 拒", L"保留设备名"},
            {{{"dir/nul", "x"}}, L"保留名 nul 段拒", L"保留设备名"},
            {{{"a.", "x"}}, L"尾点拒", L"点/空格结尾"},
            {{{"a ", "x"}}, L"尾空格拒", L"点/空格结尾"},
            {{{"a//b", "x"}}, L"空段 // 拒", L"空路径段"},
            {{{"./a", "x"}}, L". 段拒", L".."},
            {long1, L"名 >64 字符拒", L"64 字符"},
            {{{"\xC3\xA9.txt", "x"}}, L"非 ASCII 名拒", L"非法字符"},
            {{{"e.bin", "x", 1}}, L"加密条目拒", L"加密"},
            {{{"m.bin", "x", 0, 12}}, L"压缩方法 bzip2 拒", L"压缩方法"},
            {{{"s.bin", "x", 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA1ED0000, 3}},
             L"unix 符号链接拒", L"符号链接"},
            {{{"c.bin", "x", 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x21ED0000, 3}},
             L"unix 字符设备拒", L"特殊文件"},
            {{{"o.bin", "x", 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 5}}, L"非常见 OS 拒", L"来源 OS"},
            {{{"big.bin", "x", 0, 0, 0xFFFFFFFFu, (64u << 20) + 1}}, L"单文件 >64MB 拒", L"64MB"},
            {bomb, L"总量 >256MB 拒（炸弹）", L"256MB"},
            {{{"z64.bin", "x", 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, true}},
             L"zip64 哨兵拒", L"zip64"},
            {dup, L"大小写不敏感重名拒", L"重名"},
            {many, L"条目 >2048 拒", L"2048"},
        };
        for (const auto& b : bads) {
            std::wstring e;
            bool ok = PrefZip(BuildZip(b.es), L"pkg.zip", nullptr, &e);
            CHECK(!ok && e.find(b.errSub) != std::wstring::npos, b.name);
        }
        std::wstring e2;
        CHECK(!PrefZip(std::vector<unsigned char>(100, 0x5A), L"pkg.zip", nullptr, &e2) &&
                  e2.find(L"EOCD") != std::wstring::npos,
              L"非 zip 字节流 → 缺 EOCD 拒");
    }

    // ---------- 批次 3：ExtractZipTo（真跑系统 tar.exe）+ 解出复核 ----------
    {
        std::wstring err;
        auto good = BuildZip({{"hello.txt", "hi-lingxi"}, {"data/pinyin.txt", "py-data"}});
        CHECK(WriteZip(good, L"good.zip"), L"写 good.zip");
        std::wstring ext = g_tmp + L"\\ext1";
        CHECK(MkdirP(ext), L"建解出目录");
        CHECK(ExtractZipTo(TPath(L"good.zip"), ext, nullptr, &err), L"tar.exe 解包成功");
        CHECK(ReadAll(ext + L"\\hello.txt") == "hi-lingxi" &&
                  ReadAll(ext + L"\\data\\pinyin.txt") == "py-data",
              L"解出内容与原条目一致（含子目录）");
        CHECK(VerifyExtractedDir(ext, &err), L"解出目录复核通过");
        {
            HANDLE h = CreateFileW((ext + L"\\big.bin").c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            LARGE_INTEGER li;
            li.QuadPart = (64ll << 20) + 1;
            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
            CloseHandle(h);
            CHECK(!VerifyExtractedDir(ext, &err) && err.find(L"64MB") != std::wstring::npos,
                  L"解出后实测 >64MB → 拒（预校验谎报兜底）");
            DeleteFileW((ext + L"\\big.bin").c_str());
        }
        std::wstring err2;
        CHECK(!ExtractZipTo(TPath(L"definitely_missing.zip"), g_tmp, nullptr, &err2),
              L"包不存在 → 解包失败不崩溃");
    }

    // ---------- 批次 3：DownloadToFile 白名单前置拒绝（不出网）----------
    {
        std::wstring err;
        CHECK(!DownloadToFile(L"https://evil.com/a.zip", TPath(L"d.bin"), kMaxDownloadBytes,
                              nullptr, nullptr, nullptr, &err) &&
                  err.find(L"白名单") != std::wstring::npos,
              L"白名单外域 → 拒绝且不出网");
        CHECK(!DownloadToFile(L"http://github.com/a.zip", TPath(L"d.bin"), kMaxDownloadBytes,
                              nullptr, nullptr, nullptr, &err),
              L"http 明文 → 拒绝");
    }

    // ---------- 批次 4：ValidateStagingPath（T10）----------
    std::wstring bw = g_tmp + L"\\binwork";
    {
        SemVer v;
        std::wstring err;
        CHECK(MkdirP(bw + L"\\staging\\1.2.3"), L"建 binwork\\staging\\1.2.3");
        CHECK(ValidateStagingPath(bw + L"\\staging\\1.2.3", bw, &v, &err) && SemEq(v, 1, 2, 3),
              L"合法 staging 通过并解出版本");
        CHECK(ValidateStagingPath(bw + L"\\StAgInG\\1.2.3", bw, &v, &err),
              L"父目录大小写不敏感比较通过（Windows FS 本不敏感，判定逻辑须一致）");
        CHECK(!ValidateStagingPath(bw + L"\\staging\\notaversion", bw, &v, &err) &&
                  err.find(L"版本号") != std::wstring::npos,
              L"子目录名非法版本 → 拒");
        CHECK(MkdirP(g_tmp + L"\\elsewhere\\1.2.3"), L"建 elsewhere\\1.2.3");
        CHECK(!ValidateStagingPath(g_tmp + L"\\elsewhere\\1.2.3", bw, &v, &err) &&
                  err.find(L"之下") != std::wstring::npos,
              L"不在 bin\\work\\staging 之下 → 拒（任意路径提权防线）");
        CHECK(!ValidateStagingPath(bw + L"\\staging\\..\\evil\\1.2.3", bw, &v, &err),
              L"路径穿越规范化后仍被拒");
        CHECK(MkdirP(bw + L"\\staging\\1.2.3\\sub"), L"建 staging\\1.2.3\\sub");
        CHECK(!ValidateStagingPath(bw + L"\\staging\\1.2.3\\sub", bw, &v, &err),
              L"非直接子目录 → 拒");
        CHECK(!ValidateStagingPath(bw + L"\\staging\\5.5.5", bw, &v, &err) &&
                  err.find(L"不存在") != std::wstring::npos,
              L"目录不存在 → 拒");
    }

    // ---------- 批次 4：BuildReplacePlan（双位缺一不可）----------
    std::wstring stg = g_tmp + L"\\int\\staging\\9.9.9";
    {
        std::vector<FileOp> plan;
        std::wstring err;
        CHECK(MkdirP(stg + L"\\x64\\data") && MkdirP(stg + L"\\x86"), L"建 staging 双树");
        CHECK(WriteStr(stg + L"\\x64\\ImeCore.dll", "NEW64") &&
                  WriteStr(stg + L"\\x64\\data\\pinyin.txt", "PY") &&
                  WriteStr(stg + L"\\x86\\ImeCore.dll", "NEW32"),
              L"写 staging 内容");
        CHECK(BuildReplacePlan(stg, g_tmp + L"\\int\\bin", &plan, &err) && plan.size() == 3,
              L"计划 3 文件（x64 两件 + x86 一件）");
        bool d64 = false, d32 = false, sub = false;
        for (const auto& op : plan) {
            if (op.rel == L"ImeCore.dll" && op.dst.find(L"\\Debug\\") != std::wstring::npos) d64 = true;
            if (op.rel == L"ImeCore.dll" && op.dst.find(L"\\Debug32\\") != std::wstring::npos) d32 = true;
            if (op.rel == L"data\\pinyin.txt") sub = true;
        }
        CHECK(d64 && d32, L"同一 rel 映射到 Debug 与 Debug32 两侧");
        CHECK(sub, L"子目录相对路径保留");
        std::wstring no86 = g_tmp + L"\\int\\no86";
        CHECK(MkdirP(no86 + L"\\x64") && WriteStr(no86 + L"\\x64\\ImeCore.dll", "X"),
              L"建 no86（只有 x64 树）");
        CHECK(!BuildReplacePlan(no86, g_tmp + L"\\int\\bin", &plan, &err) &&
                  err.find(L"x86") != std::wstring::npos,
              L"缺 x86 子树 → 拒（漏 32 位历史教训）");
        std::wstring e86 = g_tmp + L"\\int\\empty86";
        CHECK(MkdirP(e86 + L"\\x64") && MkdirP(e86 + L"\\x86") &&
                  WriteStr(e86 + L"\\x64\\ImeCore.dll", "X") &&
                  !BuildReplacePlan(e86, g_tmp + L"\\int\\bin", &plan, &err) &&
                  err.find(L"32 位包内容为空") != std::wstring::npos,
              L"x86 空树 → 拒");
        std::wstring nodll = g_tmp + L"\\int\\nodll";
        CHECK(MkdirP(nodll + L"\\x64") && MkdirP(nodll + L"\\x86") &&
                  WriteStr(nodll + L"\\x64\\a.txt", "1") &&
                  WriteStr(nodll + L"\\x86\\b.txt", "2") &&
                  !BuildReplacePlan(nodll, g_tmp + L"\\int\\bin", &plan, &err) &&
                  err.find(L"ImeCore") != std::wstring::npos,
              L"x64 缺 ImeCore.dll → 拒（重注册依赖）");
    }

    // ---------- 批次 4：RunImecheck / RunVersionProbe 真实进程路径 ----------
    {
        std::wstring err;
        CHECK(WriteStr(TPath(L"ime_ok.ps1"), "exit 0\n"), L"写 ime_ok.ps1");
        CHECK(WriteStr(TPath(L"ime_bad.ps1"), "exit 3\n"), L"写 ime_bad.ps1");
        CHECK(RunImecheck(TPath(L"ime_ok.ps1"), &err), L"imecheck 真跑 powershell：退出码 0 通过");
        CHECK(!RunImecheck(TPath(L"ime_bad.ps1"), &err) &&
                  err.find(L"退出码") != std::wstring::npos,
              L"imecheck 非 0 → 拒（不采信自报）");
        CHECK(!RunImecheck(TPath(L"no_such.ps1"), &err), L"imecheck 脚本缺失 → 拒");
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
        std::wstring selfCopy = TPath(L"probe_self.exe");
        CHECK(CopyFileW(self, selfCopy.c_str(), FALSE), L"复制自身为探针桩");
        CHECK(RunVersionProbe(selfCopy, L"9.9.9", &err),
              L"探针真跑：无 BOM UTF-16LE 输出解码匹配");
        CHECK(!RunVersionProbe(selfCopy, L"8.8.8", &err) &&
                  err.find(L"不含") != std::wstring::npos,
              L"探针版本不符 → 拒");
        CHECK(!RunVersionProbe(TPath(L"definitely_missing.exe"), L"9.9.9", &err),
              L"探针 exe 缺失 → 拒不崩溃");
    }

    // ---------- 批次 4：ApplyUpdate 全流程（假 bin 树：替换→自验→回滚）----------
    {
        std::wstring fakeBin = g_tmp + L"\\int\\bin";
        CHECK(MkdirP(fakeBin + L"\\Debug\\data") && MkdirP(fakeBin + L"\\Debug32"), L"建假 bin 双目录");
        CHECK(WriteStr(fakeBin + L"\\Debug\\ImeCore.dll", "OLD64") &&
                  WriteStr(fakeBin + L"\\Debug\\LingxiUI.exe", "OLDUI") &&
                  WriteStr(fakeBin + L"\\Debug\\extra.txt", "keep") &&
                  WriteStr(fakeBin + L"\\Debug32\\ImeCore.dll", "OLD32"),
              L"假 bin 旧件就绪（含计划外 extra.txt）");
        // staging 的 LingxiUI.exe = 自身副本（真探针可跑）
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
        CHECK(CopyFileW(self, (stg + L"\\x64\\LingxiUI.exe").c_str(), FALSE), L"staging 放探针桩 exe");

        ApplyHooks hk;
        hk.reRegister = StubReRegister;  // imecheck/probe 走真实实现
        ApplyRequest req;
        req.binRoot = fakeBin;
        req.stagingDir = stg;
        req.versionStr = L"9.9.9";
        req.imecheckScript = TPath(L"ime_ok.ps1");
        req.doRegister = true;
        std::wstring err, log;
        CHECK(ApplyUpdate(req, hk, &err, &log), L"全流程成功（真替换+真 imecheck+真探针+哈希自验）");
        CHECK(g_regCalls == 1 && g_regDll64 == fakeBin + L"\\Debug\\ImeCore.dll" &&
                  g_regDll32 == fakeBin + L"\\Debug32\\ImeCore.dll",
              L"重注册收到双侧 DLL 路径");
        CHECK(ReadAll(fakeBin + L"\\Debug\\ImeCore.dll") == "NEW64" &&
                  ReadAll(fakeBin + L"\\Debug32\\ImeCore.dll") == "NEW32",
              L"x64 与 x86 两份都换成新件（缺一不可）");
        CHECK(ReadAll(fakeBin + L"\\Debug\\data\\pinyin.txt") == "PY" &&
                  ReadAll(fakeBin + L"\\Debug\\extra.txt") == "keep",
              L"子目录新建 + 计划外文件不动");
        CHECK(OldestPrevContent(fakeBin + L"\\Debug\\ImeCore.dll") == "OLD64" &&
                  CountPrev(fakeBin + L"\\Debug\\ImeCore.dll") >= 1,
              L"旧件保留 .prev-<ts> 快照且内容=替换前旧件");
        CHECK(log.find(L"自验 3/3") != std::wstring::npos, L"log 含三项自验全过");

        // 幂等：再跑一次同样成功
        std::wstring err2;
        CHECK(ApplyUpdate(req, hk, &err2, nullptr), L"幂等重跑仍成功");

        // 自验失败（探针版本不符）→ 整体回滚
        ApplyRequest reqBad = req;
        reqBad.versionStr = L"8.8.8";
        std::wstring err3;
        CHECK(!ApplyUpdate(reqBad, hk, &err3, nullptr) &&
                  err3.find(L"自验失败（探针") != std::wstring::npos &&
                  err3.find(L"已整体回滚") != std::wstring::npos,
              L"探针不符 → 回滚（不采信步骤返回值，T12）");
        CHECK(ReadAll(fakeBin + L"\\Debug\\ImeCore.dll") == "NEW64" &&
                  ReadAll(fakeBin + L"\\Debug32\\ImeCore.dll") == "NEW32",
              L"回滚后磁盘仍是上一轮新件（无半替换态）");

        // 重注册失败 → 回滚
        g_regOk = false;
        std::wstring err4;
        CHECK(!ApplyUpdate(req, hk, &err4, nullptr) &&
                  err4.find(L"TSF 重注册失败") != std::wstring::npos,
              L"重注册失败 → 整体回滚");
        g_regOk = true;

        // imecheck 失败（桩）→ 回滚
        g_imeOk = false;
        ApplyRequest req2 = req;
        req2.imecheckScript = TPath(L"whatever.ps1");
        std::wstring err5;
        CHECK(!ApplyUpdate(req2, hk, &err5, nullptr) &&
                  err5.find(L"自验失败（imecheck）") != std::wstring::npos,
              L"imecheck 失败 → 回滚");
        g_imeOk = true;

        // 复制中途失败（路径段被同名文件占用）→ 全量回滚
        std::wstring stg2 = g_tmp + L"\\int\\stg2";
        CHECK(MkdirP(stg2 + L"\\x64\\sub") && MkdirP(stg2 + L"\\x86"), L"建 stg2");
        CHECK(WriteStr(stg2 + L"\\x64\\ImeCore.dll", "N6") &&
                  WriteStr(stg2 + L"\\x64\\sub\\f.txt", "F") &&
                  WriteStr(stg2 + L"\\x86\\ImeCore.dll", "N3"),
              L"stg2 含 sub/f.txt（与假 bin 的 sub 文件冲突）");
        CHECK(WriteStr(fakeBin + L"\\Debug\\sub", "iam-a-file"), L"假 bin Debug 放同名文件 sub");
        ApplyRequest req3 = req;
        req3.stagingDir = stg2;
        std::wstring err6;
        CHECK(!ApplyUpdate(req3, hk, &err6, nullptr) &&
                  err6.find(L"同名文件占用") != std::wstring::npos,
              L"中途建目录失败 → 拒并回滚");
        CHECK(ReadAll(fakeBin + L"\\Debug\\ImeCore.dll") == "NEW64" &&
                  ReadAll(fakeBin + L"\\Debug\\sub") == "iam-a-file",
              L"失败后旧态完整恢复（要么全换要么全回滚，T9）");
    }

    // ---------- T2：回滚不完整 → pending-rollback 标记 → 启动自检补做两分支 ----------
    {
        std::wstring t2bin = g_tmp + L"\\t2\\bin";
        std::wstring t2stg = g_tmp + L"\\t2\\stg";
        CHECK(MkdirP(t2bin + L"\\Debug") && MkdirP(t2bin + L"\\Debug32") &&
                  MkdirP(t2stg + L"\\x64") && MkdirP(t2stg + L"\\x86"), L"T2 假树就绪");
        WriteStr(t2bin + L"\\Debug\\ImeCore.dll", "T2OLD64");
        WriteStr(t2bin + L"\\Debug32\\ImeCore.dll", "T2OLD32");
        WriteStr(t2stg + L"\\x64\\ImeCore.dll", "T2NEW64");
        WriteStr(t2stg + L"\\x86\\ImeCore.dll", "T2NEW32");
        const std::wstring marker = PendingRollbackMarkerPath(t2bin);

        ApplyHooks hk2;
        hk2.reRegister = StubReRegister;
        hk2.imecheck = StubImecheckLockDst; // 复制后锁新件 + 报败 → 回滚不完整
        g_t2LockPath = t2bin + L"\\Debug\\ImeCore.dll";
        ApplyRequest rq;
        rq.binRoot = t2bin; rq.stagingDir = t2stg; rq.versionStr = L"9.9.9";
        rq.imecheckScript = L"x.ps1"; rq.doRegister = true;
        std::wstring err, lg;
        bool ok = ApplyUpdate(rq, hk2, &err, &lg);
        std::string mark = ReadAll(marker);
        CHECK(!ok && err.find(L"回滚不完整") != std::wstring::npos &&
                  mark.find("copied=") != std::string::npos &&
                  mark.find("ImeCore.dll") != std::string::npos,
              L"T2a 回滚不完整 → pending-rollback 标记已写且含目标路径");
        CHECK(ReadAll(t2bin + L"\\Debug\\ImeCore.dll") == "T2NEW64" &&
                  ReadAll(t2bin + L"\\Debug32\\ImeCore.dll") == "T2OLD32",
              L"T2a 中间态实证：x64 新件残留（被占删不掉），x86 已回滚");

        // 分支一：占用未解 → 自检补做失败，标记保留不静默
        std::wstring healLog;
        CHECK(!TryPendingRollbackSelfHeal(t2bin, &healLog) &&
                  GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES &&
                  healLog.find(L"补做未竟") != std::wstring::npos,
              L"T2b 占用中自检：补做失败、标记保留、日志明示");
        // 分支二：占用解除 → 补做成功：新件删净、旧件改回、标记清除
        if (g_t2Lock) { CloseHandle(g_t2Lock); g_t2Lock = NULL; }
        std::wstring healLog2;
        CHECK(TryPendingRollbackSelfHeal(t2bin, &healLog2) &&
                  ReadAll(t2bin + L"\\Debug\\ImeCore.dll") == "T2OLD64" &&
                  GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"T2c 解锁后自检：补做成功（旧件改回+标记清除+幂等消化部分回滚）");
        // 幂等：标记已无 → 自检 no-op
        CHECK(TryPendingRollbackSelfHeal(t2bin, nullptr), L"T2d 无标记自检 no-op");
    }

    // ---------- T3：.prev-* 快照清理（保留最近 2 套，删最旧；失败只记日志）----------
    {
        std::wstring t3bin = g_tmp + L"\\t3\\bin";
        CHECK(MkdirP(t3bin + L"\\Debug") && MkdirP(t3bin + L"\\Debug32"), L"T3 假树就绪");
        WriteStr(t3bin + L"\\Debug\\a.dll.prev-100", "S1");
        WriteStr(t3bin + L"\\Debug32\\a.dll.prev-100", "X1");
        WriteStr(t3bin + L"\\Debug\\a.dll.prev-200", "S2");
        WriteStr(t3bin + L"\\Debug\\b.dll.prev-200", "S2b");     // 同 ts 一套两件
        WriteStr(t3bin + L"\\Debug\\a.dll.prev-300", "S3");
        WriteStr(t3bin + L"\\Debug\\a.dll.prev-300-2", "S3q");   // 同秒撞名同属一套
        WriteStr(t3bin + L"\\Debug32\\a.dll.prev-300", "X3");
        std::wstring clog;
        size_t del = CleanupOldPrevSnapshots(t3bin, 2, &clog);
        CHECK(del == 2 &&
                  GetFileAttributesW((t3bin + L"\\Debug\\a.dll.prev-100").c_str()) == INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW((t3bin + L"\\Debug32\\a.dll.prev-100").c_str()) == INVALID_FILE_ATTRIBUTES &&
                  ReadAll(t3bin + L"\\Debug\\a.dll.prev-200") == "S2" &&
                  ReadAll(t3bin + L"\\Debug\\b.dll.prev-200") == "S2b" &&
                  ReadAll(t3bin + L"\\Debug\\a.dll.prev-300") == "S3" &&
                  ReadAll(t3bin + L"\\Debug\\a.dll.prev-300-2") == "S3q" &&
                  ReadAll(t3bin + L"\\Debug32\\a.dll.prev-300") == "X3",
              L"T3a 三套快照清到两套：删最旧（双侧+同 ts 组套+seq 尾件归属正确）");
        // 失败分支：最旧套一件被锁 → 删不掉只记日志保留，不影响其他
        WriteStr(t3bin + L"\\Debug\\a.dll.prev-040", "S0");
        WriteStr(t3bin + L"\\Debug\\a.dll.prev-050", "S0L");
        g_t2LockPath = t3bin + L"\\Debug\\a.dll.prev-050";
        g_t2Lock = CreateFileW(g_t2LockPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); // 拒 DELETE
        std::wstring clog2;
        size_t del2 = CleanupOldPrevSnapshots(t3bin, 2, &clog2);
        CHECK(del2 == 1 && clog2.find(L"清理失败") != std::wstring::npos &&
                  GetFileAttributesW(g_t2LockPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW((t3bin + L"\\Debug\\a.dll.prev-040").c_str()) == INVALID_FILE_ATTRIBUTES,
              L"T3b 占用件保留+记日志，其余旧套照删（清理失败不影响结果）");
        if (g_t2Lock) { CloseHandle(g_t2Lock); g_t2Lock = NULL; }
        size_t del3 = CleanupOldPrevSnapshots(t3bin, 2, nullptr);
        CHECK(del3 == 1 && GetFileAttributesW(g_t2LockPath.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"T3c 解锁后再清：残留旧件删净");
    }

    // ---------- 批次 4：VerifyInstalledHashes 直接对（篡改兜底）----------
    {
        std::wstring err;
        std::vector<FileOp> one;
        one.push_back({L"x.bin", stg + L"\\x64\\ImeCore.dll", TPath(L"vh_dst.bin")});
        CHECK(CopyFileW(one[0].src.c_str(), one[0].dst.c_str(), FALSE) &&
                  VerifyInstalledHashes(one, &err),
              L"哈希一致 → 通过");
        WriteStr(one[0].dst, "TAMPERED");
        CHECK(!VerifyInstalledHashes(one, &err) &&
                  err.find(L"不一致") != std::wstring::npos,
              L"落盘被篡改 → 拒（替换后独立复核）");
    }

    PrintW(L"\n== update_test 汇总：PASS=%d FAIL=%d ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
