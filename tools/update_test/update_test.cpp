//
// ===== 自助更新单测工装（批次 2-3：检查侧 + 下载/校验/解压）=====
// 真断言（设计 §9）：版本比较 / JSON 解析 / 资产选择 / SUMS 解析 / 白名单 / 状态机 /
// SHA256 / zip-slip 与上限（测试内自建 stored zip 构造恶意包）/ tar 解包 / 解出复核。
// 不打真实 GitHub；网络路径由批次 6 本地假服务集成测试覆盖。
//
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../common/update.h"
#include "../../common/update_fetch.h"
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

int wmain() {
    PrintW(L"== update_test: 检查侧 + 下载/校验/解压（批次 2-3）==\n");

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

    PrintW(L"\n== update_test 汇总：PASS=%d FAIL=%d ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
