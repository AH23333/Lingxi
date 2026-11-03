//
// ===== 自助更新单测工装（批次 2：检查侧）=====
// 真断言（设计 §9）：版本比较 / JSON 解析 / 资产选择 / SUMS 解析 / 白名单 / 状态机。
// 不打真实 GitHub；网络路径由批次 6 本地假服务集成测试覆盖。
//
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../common/update.h"
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

int wmain() {
    PrintW(L"== update_test: 检查侧（批次 2）==\n");

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

    PrintW(L"\n== update_test 汇总：PASS=%d FAIL=%d ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
