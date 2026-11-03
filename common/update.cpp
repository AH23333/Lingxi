//
// ===== 自助更新：检查侧实现（批次 2）=====
// 对照 docs/update-design-2026-11-03.md：§2 发版契约、§3 版本比较、§4 威胁模型（T2/T3/T11）、§5 状态机。
// 零新增依赖：WinHTTP（HTTPS + 手动重定向逐跳验白名单）+ 手写最小 JSON。
//
#include "update.h"

#include <winhttp.h>

#include <cstdlib>
#include <cwctype>

#include "version.h"

#pragma comment(lib, "winhttp.lib")

namespace lingxi {
namespace updater {

// ---------------------------------------------------------------- 语义化版本

bool ParseSemVer(const std::wstring& text, SemVer* out) {
    if (!out) return false;
    size_t i = 0, n = text.size();
    while (i < n && iswspace(text[i])) i++;
    size_t j = n;
    while (j > i && iswspace(text[j - 1])) j--;
    std::wstring s = text.substr(i, j - i);
    if (s.empty()) return false;
    if (s[0] == L'v' || s[0] == L'V') s.erase(s.begin());
    if (s.empty()) return false;

    // 只允许 [0-9.]，出现 - + 字母等一律拒（预发布/构建后缀本期不更）。
    for (wchar_t c : s) {
        if (c != L'.' && (c < L'0' || c > L'9')) return false;
    }
    int seg[3] = {0, 0, 0};
    size_t pos = 0;
    int count = 0;
    while (pos <= s.size()) {
        size_t dot = s.find(L'.', pos);
        if (dot == std::wstring::npos) dot = s.size();
        std::wstring part = s.substr(pos, dot - pos);
        if (part.empty()) return false;         // 空段（"1..2"、尾点）
        if (part.size() > 7) return false;      // 防溢出：单段最多 7 位
        if (count >= 3) return false;           // 4 段以上拒（先判再写，防越界）
        if (part[0] == L'0' && part.size() > 1) return false; // 前导零歧义拒收
        seg[count] = _wtoi(part.c_str());
        if (seg[count] < 0) return false;
        count++;
        pos = dot + 1;
        if (dot == s.size()) break;
    }
    out->major = seg[0];
    out->minor = seg[1];
    out->patch = seg[2];
    return true;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

SemVer LocalVersion() {
    SemVer v;
    v.major = LINGXI_VERSION_MAJOR;
    v.minor = LINGXI_VERSION_MINOR;
    v.patch = LINGXI_VERSION_PATCH;
    return v;
}

// ---------------------------------------------------------------- UTF-8

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], need);
    return w;
}

// ---------------------------------------------------------------- 最小 JSON
// 只解析 releases/latest 需要的结构：对象/数组/字符串(含 \u 转义)/数字/布尔/null。
// 深度上限防恶意递归。

namespace {

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<std::pair<std::string, JVal>> members; // Obj
    std::vector<JVal> elems;                            // Arr

    const JVal* find(const char* key) const {
        if (type != Obj) return nullptr;
        for (const auto& kv : members)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct JReader {
    const char* p;
    const char* end;
    bool ok = true;

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool Eat(char c) {
        SkipWs();
        if (p < end && *p == c) { p++; return true; }
        ok = false;
        return false;
    }
    bool ParseValue(JVal* v, int depth);
    bool ParseString(std::string* out);
};

static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

bool JReader::ParseString(std::string* out) {
    SkipWs();
    if (p >= end || *p != '"') { ok = false; return false; }
    p++;
    while (p < end) {
        char c = *p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= end) break;
            char e = *p++;
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    if (end - p < 4) { ok = false; return false; }
                    auto hex4 = [&]() -> unsigned {
                        unsigned v = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = p[i];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                            else return 0xFFFFFFFFu;
                        }
                        return v;
                    };
                    unsigned cp = hex4();
                    if (cp == 0xFFFFFFFFu) { ok = false; return false; }
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) { // 代理对
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u') { ok = false; return false; }
                        p += 2;
                        unsigned lo = hex4();
                        p += 4;
                        if (lo < 0xDC00 || lo > 0xDFFF) { ok = false; return false; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(*out, cp);
                    break;
                }
                default: ok = false; return false;
            }
        } else if ((unsigned char)c < 0x20) {
            ok = false; return false; // 控制字符裸出
        } else {
            out->push_back(c);
        }
    }
    ok = false;
    return false;
}

bool JReader::ParseValue(JVal* v, int depth) {
    if (depth > 48) { ok = false; return false; }
    SkipWs();
    if (p >= end) { ok = false; return false; }
    char c = *p;
    if (c == '{') {
        p++;
        v->type = JVal::Obj;
        SkipWs();
        if (p < end && *p == '}') { p++; return true; }
        while (p < end) {
            std::string key;
            if (!ParseString(&key)) return false;
            if (!Eat(':')) return false;
            JVal child;
            if (!ParseValue(&child, depth + 1)) return false;
            v->members.emplace_back(std::move(key), std::move(child));
            SkipWs();
            if (p < end && *p == ',') { p++; continue; }
            return Eat('}');
        }
        ok = false;
        return false;
    }
    if (c == '[') {
        p++;
        v->type = JVal::Arr;
        SkipWs();
        if (p < end && *p == ']') { p++; return true; }
        while (p < end) {
            JVal child;
            if (!ParseValue(&child, depth + 1)) return false;
            v->elems.push_back(std::move(child));
            SkipWs();
            if (p < end && *p == ',') { p++; continue; }
            return Eat(']');
        }
        ok = false;
        return false;
    }
    if (c == '"') {
        v->type = JVal::Str;
        return ParseString(&v->str);
    }
    if (c == 't') { // true
        if (end - p < 4 || memcmp(p, "true", 4) != 0) { ok = false; return false; }
        p += 4; v->type = JVal::Bool; v->b = true; return true;
    }
    if (c == 'f') {
        if (end - p < 5 || memcmp(p, "false", 5) != 0) { ok = false; return false; }
        p += 5; v->type = JVal::Bool; v->b = false; return true;
    }
    if (c == 'n') {
        if (end - p < 4 || memcmp(p, "null", 4) != 0) { ok = false; return false; }
        p += 4; v->type = JVal::Null; return true;
    }
    // 数字
    const char* start = p;
    if (p < end && (*p == '-' || *p == '+')) p++;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                       *p == '-' || *p == '+'))
        p++;
    if (p == start) { ok = false; return false; }
    v->type = JVal::Num;
    v->num = strtod(std::string(start, (size_t)(p - start)).c_str(), nullptr);
    return true;
}

} // namespace

bool ParseReleaseJson(const std::string& jsonUtf8, ReleaseInfo* out, std::wstring* err) {
    if (!out) return false;
    JReader r{jsonUtf8.data(), jsonUtf8.data() + jsonUtf8.size()};
    JVal root;
    if (!r.ParseValue(&root, 0) || !r.ok || root.type != JVal::Obj) {
        if (err) *err = L"release 数据解析失败（JSON 非法）";
        return false;
    }
    const JVal* tag = root.find("tag_name");
    if (!tag || tag->type != JVal::Str || tag->str.empty()) {
        if (err) *err = L"release 数据缺少 tag_name";
        return false;
    }
    const JVal* assets = root.find("assets");
    if (!assets || assets->type != JVal::Arr) {
        if (err) *err = L"release 数据缺少 assets 列表";
        return false;
    }
    out->tag = Utf8ToWide(tag->str);
    const JVal* body = root.find("body");
    out->notes = (body && body->type == JVal::Str) ? Utf8ToWide(body->str) : L"";
    out->assets.clear();
    for (const JVal& a : assets->elems) {
        if (a.type != JVal::Obj) continue;
        const JVal* n = a.find("name");
        const JVal* u = a.find("browser_download_url");
        if (!n || n->type != JVal::Str || !u || u->type != JVal::Str) continue;
        ReleaseAsset ra;
        ra.name = Utf8ToWide(n->str);
        ra.downloadUrl = Utf8ToWide(u->str);
        const JVal* sz = a.find("size");
        if (sz && sz->type == JVal::Num) ra.size = (long long)sz->num;
        out->assets.push_back(std::move(ra));
    }
    return true;
}

// ---------------------------------------------------------------- 契约资产选择

static const wchar_t* kAssetX64 = L"LingxiIme-win-x64.zip";
static const wchar_t* kAssetX86 = L"LingxiIme-win-x86.zip";
static const wchar_t* kAssetSums = L"SHA256SUMS.txt";

bool SelectAssets(const ReleaseInfo& rel, SelectedAssets* out, std::wstring* reason) {
    if (!out) return false;
    const ReleaseAsset* x64 = nullptr;
    const ReleaseAsset* x86 = nullptr;
    const ReleaseAsset* sums = nullptr;
    for (const auto& a : rel.assets) {
        if (a.name == kAssetX64) x64 = &a;
        else if (a.name == kAssetX86) x86 = &a;
        else if (a.name == kAssetSums) sums = &a;
    }
    if (!x64 || !x86 || !sums) {
        if (reason) {
            std::wstring missing;
            if (!x64) missing += L" LingxiIme-win-x64.zip";
            if (!x86) missing += L" LingxiIme-win-x86.zip";
            if (!sums) missing += L" SHA256SUMS.txt";
            *reason = L"发布包不完整：缺" + missing + L"（按发版契约判不可用，不降级直装）";
        }
        return false;
    }
    out->x64 = *x64;
    out->x86 = *x86;
    out->sums = *sums;
    return true;
}

// ---------------------------------------------------------------- 域名白名单 + HTTP

// ===== 批次 6 演练钩子：LINGXI_UPDATE_TEST_BASE =====
// 仅当该环境变量为 http(s)://127.0.0.1[:port] 或 http(s)://localhost[:port] 时生效，
// 供本地假发布服务（tools/update_test/fake_release_server.py）驱动全链路演练。
// 非回环 / 非法值 → 整体忽略，生产行为与不设完全一致；白名单旁路也只放行
// 回环 host，重定向逐跳复核口径不变（T1/T2 对生产网络零放宽）。
const std::wstring* TestBaseUrl() {
    static const std::wstring base = [] {
        wchar_t buf[256] = {};
        DWORD n = GetEnvironmentVariableW(L"LINGXI_UPDATE_TEST_BASE", buf, ARRAYSIZE(buf));
        if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
        URL_COMPONENTS uc = {};
        wchar_t host[256] = {};
        uc.dwStructSize = sizeof(uc);
        uc.lpszHostName = host;
        uc.dwHostNameLength = ARRAYSIZE(host);
        // 无路径形态（http://127.0.0.1:8765）CrackUrl 可能拒 → 补 "/" 重试一次。
        if (!WinHttpCrackUrl(buf, 0, 0, &uc)) {
            size_t len = wcslen(buf);
            if (len + 2 >= ARRAYSIZE(buf)) return std::wstring();
            buf[len] = L'/';
            buf[len + 1] = 0;
            uc.dwStructSize = sizeof(uc);
            uc.lpszHostName = host;
            uc.dwHostNameLength = ARRAYSIZE(host);
            if (!WinHttpCrackUrl(buf, 0, 0, &uc)) return std::wstring();
        }
        if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
            return std::wstring();
        if (_wcsicmp(host, L"127.0.0.1") != 0 && _wcsicmp(host, L"localhost") != 0)
            return std::wstring(); // 非回环绝不开旁路
        std::wstring s(buf);
        while (!s.empty() && s.back() == L'/') s.pop_back();
        return s;
    }();
    return base.empty() ? nullptr : &base;
}

bool IsUrlAllowed(const std::wstring& url) {
    static const wchar_t* kAllow[] = {L"api.github.com", L"github.com",
                                      L"objects.githubusercontent.com"};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    if (url.find(L'@') != std::wstring::npos) return false; // userinfo 夹带（T2 走私面）
    const bool loopback = _wcsicmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        // 仅 HTTPS（T1/T2）；http 只在演练钩子激活且 host 为回环时放行
        if (!(TestBaseUrl() && uc.nScheme == INTERNET_SCHEME_HTTP && loopback)) return false;
    }
    for (const wchar_t* h : kAllow)
        if (_wcsicmp(host, h) == 0) return true; // 精确匹配，子域伪装拒
    if (TestBaseUrl() && loopback) return true;   // 演练旁路：只此一条，回环限定
    return false;
}

HttpReply HttpGet(const std::wstring& url, size_t maxBytes, const volatile LONG* cancelFlag) {
    HttpReply reply;
    if (!IsUrlAllowed(url)) {
        reply.err = L"URL 不在 HTTPS 域名白名单内，已拒绝";
        return reply;
    }
    HINTERNET sess = WinHttpOpen(L"LingxiImeUpdate/" LINGXI_VERSION_STRING,
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) { reply.err = L"网络初始化失败"; return reply; }
    // 禁自动重定向：手动逐跳取 Location 并复核白名单（T2）。
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(sess, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    DWORD tmo[] = {10000, 10000, 30000, 30000}; // resolve/connect/send/receive
    WinHttpSetTimeouts(sess, (int)tmo[0], (int)tmo[1], (int)tmo[2], (int)tmo[3]);

    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    INTERNET_PORT port = 443;
    {
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof(uc);
        uc.lpszHostName = host;
        uc.dwHostNameLength = ARRAYSIZE(host);
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = ARRAYSIZE(path);
        WinHttpCrackUrl(url.c_str(), 0, 0, &uc);
        port = uc.nPort;
    }

    std::wstring cur = url;
    for (int hop = 0; hop <= 5; hop++) {
        if (cancelFlag && *cancelFlag) { reply.err = L"已取消"; WinHttpCloseHandle(sess); return reply; }
        if (!IsUrlAllowed(cur)) {
            reply.err = L"重定向目标不在白名单域名，已拒绝";
            WinHttpCloseHandle(sess);
            return reply;
        }
        // 重解析当前 URL（Location 可能换 host/path）。
        URL_COMPONENTS uc = {};
        wchar_t h2[256] = {};
        wchar_t p2[2048] = {};
        uc.dwStructSize = sizeof(uc);
        uc.lpszHostName = h2;
        uc.dwHostNameLength = ARRAYSIZE(h2);
        uc.lpszUrlPath = p2;
        uc.dwUrlPathLength = ARRAYSIZE(p2);
        if (!WinHttpCrackUrl(cur.c_str(), 0, 0, &uc)) {
            reply.err = L"URL 解析失败";
            break;
        }
        HINTERNET conn = WinHttpConnect(sess, h2, uc.nPort, 0);
        if (!conn) { reply.err = L"连接失败（DNS/网络）"; break; }
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", p2, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(conn); reply.err = L"请求创建失败"; break; }
        WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json\r\n"
                                      L"X-GitHub-Api-Version: 2022-11-28\r\n",
                                 (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        BOOL got = sent && WinHttpReceiveResponse(req, nullptr);
        if (!got) {
            DWORD e = GetLastError();
            reply.err = L"请求失败（WinHTTP " + std::to_wstring(e) +
                        (e == ERROR_WINHTTP_TIMEOUT ? L"，超时" : L"") + L"）";
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }
        DWORD status = 0, cb = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX);
        reply.status = status;

        if (status >= 300 && status < 400) {
            wchar_t loc[2048] = {};
            cb = sizeof(loc);
            BOOL has = WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                           WINHTTP_HEADER_NAME_BY_INDEX, loc, &cb,
                                           WINHTTP_NO_HEADER_INDEX);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            if (!has) { reply.err = L"重定向缺少 Location"; break; }
            std::wstring l(loc);
            if (l.compare(0, 8, L"https://") == 0) {
                cur = l;
                continue;
            }
            if (!l.empty() && l[0] == L'/') { // 相对路径：拼回当前 host
                URL_COMPONENTS bu = {};
                wchar_t bh[256] = {};
                bu.dwStructSize = sizeof(bu);
                bu.lpszHostName = bh;
                bu.dwHostNameLength = ARRAYSIZE(bh);
                WinHttpCrackUrl(cur.c_str(), 0, 0, &bu);
                cur = L"https://" + std::wstring(bh) + l;
                continue;
            }
            reply.err = L"重定向目标非法（仅允许 https 绝对/根路径）";
            break;
        }

        // 读 body（体积上限 + 取消位轮询，T11）。
        reply.body.clear();
        bool overflow = false;
        for (;;) {
            if (cancelFlag && *cancelFlag) { reply.err = L"已取消"; reply.status = 0; break; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { reply.err = L"读取中断"; break; }
            if (avail == 0) break;
            char buf[65536];
            DWORD chunk = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD done = 0;
            if (!WinHttpReadData(req, buf, chunk, &done)) { reply.err = L"读取中断"; break; }
            if (reply.body.size() + done > maxBytes) { overflow = true; break; }
            reply.body.append(buf, done);
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        if (overflow) { reply.err = L"响应超过体积上限，已中止"; reply.status = 0; }
        break;
    }
    WinHttpCloseHandle(sess);
    return reply;
}

// ---------------------------------------------------------------- SHA256SUMS 解析

bool ParseSha256Sums(const std::string& text,
                     std::vector<std::pair<std::wstring, std::string>>* out,
                     std::wstring* err) {
    if (!out) return false;
    out->clear();
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lineNo++;
        if (line.empty()) continue;
        // 格式：<hex64> SP+ [*]?name
        size_t sp = line.find(' ');
        if (sp == std::string::npos || sp != 64) {
            if (err) *err = L"SHA256SUMS 第 " + std::to_wstring(lineNo) + L" 行格式非法";
            return false;
        }
        std::string hex = line.substr(0, 64);
        for (char c : hex) {
            bool okc = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!okc) {
                if (err) *err = L"SHA256SUMS 第 " + std::to_wstring(lineNo) + L" 行含非法 hex";
                return false;
            }
        }
        for (auto& c : hex) c = (char)tolower(c);
        std::string name = line.substr(sp + 1);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (!name.empty() && name.front() == '*') name.erase(name.begin()); // binary 标记
        if (name.empty()) {
            if (err) *err = L"SHA256SUMS 第 " + std::to_wstring(lineNo) + L" 行缺文件名";
            return false;
        }
        out->emplace_back(Utf8ToWide(name), hex);
    }
    if (out->empty()) {
        if (err) *err = L"SHA256SUMS 为空";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 状态机

const wchar_t* StateText(State s) {
    switch (s) {
        case State::NotConfigured:    return L"未配置更新源";
        case State::Idle:             return L"尚未检查";
        case State::Checking:         return L"检查中";
        case State::NotPublished:     return L"尚未发布任何版本";
        case State::UpToDate:         return L"已是最新";
        case State::NewVersion:       return L"发现新版";
        case State::Incomplete:       return L"发布包不完整";
        case State::Downloading:      return L"下载中";
        case State::Verifying:        return L"校验中";
        case State::Staged:           return L"待替换";
        case State::Replacing:        return L"替换中";
        case State::VerifyingInstall: return L"自验中";
        case State::AwaitingRestart:  return L"等待重启生效";
        case State::RollingBack:      return L"回滚中";
        case State::Failed:           return L"检查失败";
    }
    return L"未知状态";
}

State ProcessLatestReply(const HttpReply& reply, const SemVer& local,
                         ReleaseInfo* rel, SemVer* remote, std::wstring* err) {
    if (reply.status == 404) return State::NotPublished; // 正常态（设计 §5）
    if (reply.status == 0) {
        if (err) *err = reply.err.empty() ? L"网络失败" : reply.err;
        return State::Failed;
    }
    if (reply.status == 403 || reply.status == 429) {
        if (err) *err = L"GitHub 限流（HTTP " + std::to_wstring(reply.status) + L"），请稍后重试";
        return State::Failed;
    }
    if (reply.status != 200) {
        if (err) *err = L"GitHub 返回 HTTP " + std::to_wstring(reply.status);
        return State::Failed;
    }
    ReleaseInfo parsed;
    if (!ParseReleaseJson(reply.body, &parsed, err)) return State::Failed;
    SemVer rv;
    if (!ParseSemVer(parsed.tag, &rv)) {
        if (err) *err = L"版本号无法解析：" + parsed.tag;
        return State::Failed;
    }
    // 资产完整性只在"确实有新版"时才是门槛（批次 6 演练抓出：旧顺序把
    // "同版本+无资产"的正常 release 误判 Incomplete——无新版可下时资产残缺无关）。
    if (CompareSemVer(rv, local) <= 0) {
        if (rel) *rel = std::move(parsed);
        if (remote) *remote = rv;
        return State::UpToDate;
    }
    SelectedAssets sel;
    std::wstring reason;
    if (!SelectAssets(parsed, &sel, &reason)) {
        if (err) *err = reason;
        return State::Incomplete;
    }
    if (rel) *rel = std::move(parsed);
    if (remote) *remote = rv;
    return State::NewVersion;
}

State CheckLatest(const std::wstring& repo, const volatile LONG* cancelFlag,
                  ReleaseInfo* rel, SemVer* remote, std::wstring* err) {
    if (repo.empty()) return State::NotConfigured; // 正常态
    // repo 必须是 owner/repo，字符集收紧（防 URL 注入/路径穿越）。
    std::wstring owner, name;
    size_t slash = repo.find(L'/');
    if (slash == std::wstring::npos || slash == 0 || slash + 1 >= repo.size() ||
        repo.find(L'/', slash + 1) != std::wstring::npos) {
        if (err) *err = L"仓库地址格式应为 owner/repo";
        return State::Failed;
    }
    for (size_t i = 0; i < repo.size(); i++) {
        wchar_t c = repo[i];
        // '/' 必须在字符集内：单斜杠结构已由上方校验钉死（owner/repo 各段无斜杠）。
        // 批次 6 演练抓出的真缺陷：旧字符集漏 '/'，一切合法 owner/repo 被误判非法。
        bool okc = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                   (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-' ||
                   c == L'/';
        if (!okc) {
            if (err) *err = L"仓库地址含非法字符";
            return State::Failed;
        }
    }
    std::wstring latestUrl = L"https://api.github.com/repos/" + repo + L"/releases/latest";
    if (const std::wstring* tb = TestBaseUrl())
        latestUrl = *tb + L"/repos/" + repo + L"/releases/latest"; // 批次 6 演练：本地假服务
    HttpReply reply = HttpGet(latestUrl, 8u * 1024u * 1024u, cancelFlag);
    return ProcessLatestReply(reply, LocalVersion(), rel, remote, err);
}

} // namespace updater
} // namespace lingxi
