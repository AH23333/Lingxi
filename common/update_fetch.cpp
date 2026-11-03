//
// ===== 自助更新：下载 / 校验 / 解压实现（批次 3）=====
// 对照 docs/update-design-2026-11-03.md §2/§4（T1/T2/T4/T5/T6/T7/T11）/§6。
// 零新增依赖：WinHTTP + CNG + 系统 tar.exe。
//
#include "update_fetch.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>

#include "update.h"
#include "version.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace lingxi {
namespace updater {

namespace {

// ---------------- 小工具 ----------------

bool ReadFileRange(const std::wstring& path, long long offset, void* buf, size_t len,
                   std::wstring* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"打开文件失败（WinAPI " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    LARGE_INTEGER li;
    li.QuadPart = offset;
    BOOL ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    size_t got = 0;
    while (ok && got < len) {
        DWORD n = 0;
        DWORD want = (DWORD)(len - got > 1u << 20 ? 1u << 20 : len - got);
        if (!ReadFile(h, (char*)buf + got, want, &n, nullptr) || n == 0) { ok = FALSE; break; }
        got += n;
    }
    CloseHandle(h);
    if (!ok || got != len) {
        if (err) *err = L"读取文件区间失败";
        return false;
    }
    return true;
}

unsigned short Le16(const unsigned char* p) { return (unsigned short)(p[0] | (p[1] << 8)); }
unsigned long Le32(const unsigned char* p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

bool IsReservedDeviceName(const std::string& seg) {
    // 段中第一个 '.' 之前的部分（大小写不敏感）命中保留名即拒。
    size_t dot = seg.find('.');
    std::string stem = seg.substr(0, dot == std::string::npos ? seg.size() : dot);
    for (auto& c : stem) c = (char)toupper((unsigned char)c);
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4 && (stem[0] == 'C' || stem[0] == 'L')) {
        bool com = stem.compare(0, 3, "COM") == 0, lpt = stem.compare(0, 3, "LPT") == 0;
        if ((com || lpt) && stem[3] >= '1' && stem[3] <= '9') return true;
    }
    return false;
}

// zip 条目名校验（设计 §2）。返回 false 并给原因。
bool ValidateZipEntryName(const std::string& name, bool* isDirOut, std::wstring* err) {
    if (name.empty()) {
        *err = L"zip 条目名为空";
        return false;
    }
    if (name.size() > kMaxZipNameLen) {
        *err = L"zip 条目名超过 64 字符";
        return false;
    }
    for (unsigned char c : name) {
        bool okc = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-' || c == '/' || c == ' ';
        if (!okc) {
            *err = L"zip 条目名含非法字符（仅允许 ASCII 字母数字 . _ - / 空格）：" + Utf8ToWide(name);
            return false;
        }
    }
    bool isDir = name.back() == '/';
    // 逐段校验（目录条目允许末尾 '/' 产生的空尾段）。
    size_t pos = 0;
    while (pos < name.size()) {
        size_t slash = name.find('/', pos);
        if (slash == std::string::npos) slash = name.size();
        std::string seg = name.substr(pos, slash - pos);
        bool last = (slash >= name.size());
        if (seg.empty()) {
            if (isDir && last) break; // 目录条目尾斜杠
            *err = L"zip 条目含空路径段（绝对路径或 //）：" + Utf8ToWide(name);
            return false;
        }
        if (seg == ".." || seg == ".") {
            *err = L"zip 条目含 . / .. 段（zip-slip）：" + Utf8ToWide(name);
            return false;
        }
        if (seg.back() == '.' || seg.back() == ' ') {
            *err = L"zip 条目段以点/空格结尾：" + Utf8ToWide(name);
            return false;
        }
        if (IsReservedDeviceName(seg)) {
            *err = L"zip 条目命中 Windows 保留设备名：" + Utf8ToWide(name);
            return false;
        }
        if (isDir && last) break;
        pos = slash + 1;
    }
    *isDirOut = isDir;
    return true;
}

// ---------------- HTTP 会话（DownloadToFile 与 HttpGet 同构）----------------

struct WinSession {
    HINTERNET sess = nullptr;
    WinSession() {
        sess = WinHttpOpen(L"LingxiImeUpdate/" LINGXI_VERSION_STRING,
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sess) return;
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; // T2：手动逐跳
        WinHttpSetOption(sess, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        WinHttpSetTimeouts(sess, 10000, 10000, 30000, 30000); // T11
    }
    ~WinSession() {
        if (sess) WinHttpCloseHandle(sess);
    }
};

} // namespace

// ================================================================ 下载

bool DownloadToFile(const std::wstring& url, const std::wstring& destPath, size_t maxBytes,
                    const volatile LONG* cancelFlag, DownloadProgressFn progress, void* ctx,
                    std::wstring* err) {
    auto fail = [&](const std::wstring& msg) {
        if (err) *err = msg;
        return false;
    };
    if (!IsUrlAllowed(url)) return fail(L"URL 不在 HTTPS 域名白名单内，已拒绝"); // 不出网

    WinSession ws;
    if (!ws.sess) return fail(L"网络初始化失败");

    std::wstring cur = url;
    for (int hop = 0; hop <= 5; hop++) {
        if (cancelFlag && *cancelFlag) return fail(L"已取消");
        if (!IsUrlAllowed(cur)) return fail(L"重定向目标不在白名单域名，已拒绝");
        URL_COMPONENTS uc = {};
        wchar_t h2[256] = {}, p2[2048] = {};
        uc.dwStructSize = sizeof(uc);
        uc.lpszHostName = h2;
        uc.dwHostNameLength = ARRAYSIZE(h2);
        uc.lpszUrlPath = p2;
        uc.dwUrlPathLength = ARRAYSIZE(p2);
        if (!WinHttpCrackUrl(cur.c_str(), 0, 0, &uc)) return fail(L"URL 解析失败");
        HINTERNET conn = WinHttpConnect(ws.sess, h2, uc.nPort, 0);
        if (!conn) return fail(L"连接失败（DNS/网络）");
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", p2, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            WinHttpCloseHandle(conn);
            return fail(L"请求创建失败");
        }
        BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        BOOL got = sent && WinHttpReceiveResponse(req, nullptr);
        if (!got) {
            DWORD e = GetLastError();
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            return fail(L"请求失败（WinHTTP " + std::to_wstring(e) +
                        (e == ERROR_WINHTTP_TIMEOUT ? L"，超时" : L"") + L"）");
        }
        DWORD status = 0, cb = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX);

        if (status >= 300 && status < 400) {
            wchar_t loc[2048] = {};
            cb = sizeof(loc);
            BOOL has = WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                           loc, &cb, WINHTTP_NO_HEADER_INDEX);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            if (!has) return fail(L"重定向缺少 Location");
            std::wstring l(loc);
            if (l.compare(0, 8, L"https://") == 0) { cur = l; continue; }
            if (!l.empty() && l[0] == L'/') {
                URL_COMPONENTS bu = {};
                wchar_t bh[256] = {};
                bu.dwStructSize = sizeof(bu);
                bu.lpszHostName = bh;
                bu.dwHostNameLength = ARRAYSIZE(bh);
                WinHttpCrackUrl(cur.c_str(), 0, 0, &bu);
                cur = L"https://" + std::wstring(bh) + l;
                continue;
            }
            return fail(L"重定向目标非法（仅允许 https 绝对/根路径）");
        }
        if (status != 200) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            return fail(L"下载失败（HTTP " + std::to_wstring(status) + L"）");
        }

        // Content-Length 预检（声明超限直接拒，不开始写盘）。
        long long declared = -1;
        {
            wchar_t cl[32] = {};
            cb = sizeof(cl);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                    cl, &cb, WINHTTP_NO_HEADER_INDEX)) {
                declared = _wcstoi64(cl, nullptr, 10);
                if (declared >= 0 && (size_t)declared > maxBytes) {
                    WinHttpCloseHandle(req);
                    WinHttpCloseHandle(conn);
                    return fail(L"下载体积超过上限，已拒绝");
                }
            }
        }

        HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            return fail(L"创建下载文件失败（WinAPI " + std::to_wstring(GetLastError()) + L"）");
        }

        long long total = 0;
        bool ioErr = false, overflow = false, canceled = false;
        std::wstring ioMsg;
        for (;;) {
            if (cancelFlag && *cancelFlag) { canceled = true; break; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                ioErr = true;
                ioMsg = L"下载中断（连接断开/超时）";
                break;
            }
            if (avail == 0) break; // 完成
            char buf[65536];
            DWORD chunk = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD done = 0;
            if (!WinHttpReadData(req, buf, chunk, &done)) {
                ioErr = true;
                ioMsg = L"下载中断（读取失败）";
                break;
            }
            if (total + (long long)done > (long long)maxBytes) {
                overflow = true;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(file, buf, done, &written, nullptr) || written != done) {
                ioErr = true;
                ioMsg = L"写入下载文件失败（磁盘/权限）";
                break;
            }
            total += done;
            if (progress && !progress(total, declared, ctx)) { canceled = true; break; }
        }
        CloseHandle(file);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);

        // 任何非成功结局：删除半成品，绝不留下可被误用的残件。
        bool ok = !ioErr && !overflow && !canceled;
        if (!ok) DeleteFileW(destPath.c_str());
        if (ioErr) return fail(ioMsg);
        if (overflow) return fail(L"下载体积超过上限，已中止并删除残件");
        if (canceled) return fail(L"已取消");
        return true;
    }
    return fail(L"重定向次数过多");
}

// ================================================================ SHA256

bool Sha256File(const std::wstring& path, std::string* hexLower, std::wstring* err) {
    if (hexLower) hexLower->clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::string hex;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        if (err) *err = L"SHA256 初始化失败";
        goto out;
    }
    {
        DWORD cbObj = 0, got = 0;
        if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbObj, sizeof(cbObj), &got, 0) != 0)
            goto out;
        std::vector<BYTE> hashObj(cbObj);
        DWORD cbHash = 0;
        if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(cbHash), &got, 0) != 0)
            goto out;
        std::vector<BYTE> digest(cbHash);
        if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), cbObj, nullptr, 0, 0) != 0) goto out;

        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (err) *err = L"打开待校验文件失败：" + path;
            goto out;
        }
        std::vector<BYTE> buf(1u << 16);
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(file, buf.data(), (DWORD)buf.size(), &n, nullptr)) {
                if (err) *err = L"读取待校验文件失败";
                goto out;
            }
            if (n == 0) break;
            if (BCryptHashData(hHash, buf.data(), n, 0) != 0) {
                if (err) *err = L"SHA256 计算失败";
                goto out;
            }
        }
        if (BCryptFinishHash(hHash, digest.data(), cbHash, 0) != 0) {
            if (err) *err = L"SHA256 收尾失败";
            goto out;
        }
        static const char kHex[] = "0123456789abcdef";
        for (BYTE b : digest) {
            hex.push_back(kHex[b >> 4]);
            hex.push_back(kHex[b & 0xF]);
        }
        ok = true;
    }
out:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    if (ok && hexLower) *hexLower = std::move(hex);
    return ok;
}

bool VerifyAgainstSums(const std::vector<std::pair<std::wstring, std::string>>& sums,
                       const std::wstring& fileName, const std::wstring& filePath,
                       std::wstring* err) {
    const std::string* want = nullptr;
    for (const auto& kv : sums)
        if (kv.first == fileName) want = &kv.second;
    if (!want) {
        if (err) *err = L"SHA256SUMS 缺少 " + fileName + L" 的校验行";
        return false;
    }
    std::string gotHex;
    if (!Sha256File(filePath, &gotHex, err)) return false;
    if (gotHex != *want) {
        if (err) *err = L"校验失败：" + fileName + L" 的 SHA256 与 SHA256SUMS 不一致（整包丢弃）";
        return false;
    }
    return true;
}

// ================================================================ zip 预校验

bool PreflightZip(const std::wstring& zipPath, std::vector<ZipEntry>* out, std::wstring* err) {
    auto fail = [&](const std::wstring& msg) {
        if (err) *err = msg;
        return false;
    };
    HANDLE h = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(L"打开 zip 失败");
    LARGE_INTEGER sz;
    BOOL gotSz = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    if (!gotSz || sz.QuadPart < 22) return fail(L"zip 过小/不可读（缺中央目录）");

    // 尾部窗口找 EOCD（签名 + 注释长度自洽才算命中，防假签名）。
    long long tailLen = sz.QuadPart < 65557 ? sz.QuadPart : 65557;
    std::vector<unsigned char> tail((size_t)tailLen);
    if (!ReadFileRange(zipPath, sz.QuadPart - tailLen, tail.data(), tail.size(), err)) return false;
    long long eocd = -1;
    for (long long i = (long long)tail.size() - 22; i >= 0; i--) {
        if (Le32(&tail[(size_t)i]) != 0x06054b50u) continue;
        unsigned short commentLen = Le16(&tail[(size_t)i + 20]);
        if ((long long)i + 22 + commentLen == (long long)tail.size()) { eocd = i; break; }
    }
    if (eocd < 0) return fail(L"zip 缺 EOCD（非 zip 或已损坏）");

    const unsigned char* e = &tail[(size_t)eocd];
    unsigned long count = Le16(e + 10);
    unsigned long cdSize = Le32(e + 12);
    unsigned long cdOffset = Le32(e + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu)
        return fail(L"zip64 不支持（发版契约要求标准 zip）");
    if (count == 0) return fail(L"zip 无条目");
    if (count > kMaxZipEntries) return fail(L"zip 条目数超过 2048 上限");
    if (cdSize > (8u << 20)) return fail(L"zip 中央目录过大");
    if ((long long)cdOffset + (long long)cdSize > sz.QuadPart)
        return fail(L"zip 中央目录越界（结构损坏）");

    std::vector<unsigned char> cd(cdSize);
    if (!ReadFileRange(zipPath, cdOffset, cd.data(), cd.size(), err)) return false;

    std::vector<ZipEntry> entries;
    std::vector<std::string> lowered;
    unsigned long long totalUncomp = 0;
    size_t pos = 0;
    for (unsigned long n = 0; n < count; n++) {
        if (pos + 46 > cd.size()) return fail(L"zip 中央目录截断");
        const unsigned char* r = &cd[pos];
        if (Le32(r) != 0x02014b50u) return fail(L"zip 中央目录签名非法");
        unsigned short madeBy = Le16(r + 4);
        unsigned short flags = Le16(r + 8);
        unsigned long method = Le16(r + 10);
        unsigned long crc = Le32(r + 16);
        unsigned long comp = Le32(r + 20);
        unsigned long uncomp = Le32(r + 24);
        unsigned short nameLen = Le16(r + 28);
        unsigned short extraLen = Le16(r + 30);
        unsigned short commentLen = Le16(r + 32);
        unsigned long extAttr = Le32(r + 38);
        if (pos + 46 + (size_t)nameLen > cd.size()) return fail(L"zip 条目名越界");
        std::string name((const char*)r + 46, nameLen);
        pos += 46 + (size_t)nameLen + extraLen + commentLen;

        ZipEntry ent;
        ent.name = name;
        ent.crc32 = crc;
        ent.compSize = comp;
        ent.uncompSize = uncomp;
        ent.method = method;

        if (flags & 0x1) {
            if (err) *err = L"zip 含加密条目（契约禁止）：" + Utf8ToWide(name);
            return false;
        }
        if (flags & 0x40) {
            if (err) *err = L"zip 含强加密条目（契约禁止）：" + Utf8ToWide(name);
            return false;
        }
        if (method != 0 && method != 8) {
            if (err) *err = L"zip 条目压缩方法不在允许集（store/deflate）：" + Utf8ToWide(name);
            return false;
        }
        if (comp == 0xFFFFFFFFu || uncomp == 0xFFFFFFFFu) {
            if (err) *err = L"zip 含 zip64 哨兵值：" + Utf8ToWide(name);
            return false;
        }
        unsigned char os = (unsigned char)(madeBy >> 8);
        if (os != 0 && os != 3) {
            if (err) *err = L"zip 条目来源 OS 非常见（DOS/unix）：" + Utf8ToWide(name);
            return false;
        }
        bool isDir = false;
        if (!ValidateZipEntryName(name, &isDir, err)) return false;
        ent.isDir = isDir;
        if (os == 3) { // unix：external attributes 高 16 位是 st_mode
            unsigned long fmt = (extAttr >> 16) & 0xF000;
            if (fmt == 0xA000) {
                if (err) *err = L"zip 含符号链接条目（契约禁止）：" + Utf8ToWide(name);
                return false;
            }
            if (fmt != 0x8000 && fmt != 0x4000) {
                if (err) *err = L"zip 含特殊文件条目（设备/socket/FIFO）：" + Utf8ToWide(name);
                return false;
            }
            if (fmt == 0x4000) isDir = true;
        }
        if (!isDir) {
            if (uncomp > kMaxFileUncompressed) {
                if (err) *err = L"zip 单文件解压后超过 64MB：" + Utf8ToWide(name);
                return false;
            }
            totalUncomp += uncomp;
            if (totalUncomp > kMaxTotalUncompressed) {
                if (err) *err = L"zip 解压后总大小超过 256MB（解压炸弹防护）";
                return false;
            }
        }
        std::string low = name;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        for (const auto& l : lowered)
            if (l == low) {
                if (err) *err = L"zip 条目重名（Windows 大小写不敏感）：" + Utf8ToWide(name);
                return false;
            }
        lowered.push_back(std::move(low));
        entries.push_back(std::move(ent));
    }

    if (out) *out = std::move(entries);
    return true;
}

// ================================================================ 解包（tar.exe）

bool ExtractZipTo(const std::wstring& zipPath, const std::wstring& destDir,
                  const volatile LONG* cancelFlag, std::wstring* err) {
    auto fail = [&](const std::wstring& msg) {
        if (err) *err = msg;
        return false;
    };
    wchar_t sysdir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sysdir, ARRAYSIZE(sysdir))) return fail(L"取系统目录失败");
    std::wstring tar = std::wstring(sysdir) + L"\\tar.exe"; // T7：绝对路径，不走 PATH 搜索
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(L"系统 tar.exe 不可用（需要 Windows 10 1803+）");

    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zipPath + L"\" -C \"" + destDir + L"\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return fail(L"创建管道失败");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    BOOL started = CreateProcessW(tar.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        return fail(L"启动 tar.exe 失败（WinAPI " + std::to_wstring(GetLastError()) + L"）");
    }

    std::string output;
    {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) output.append(buf, n);
    }
    CloseHandle(rd);

    bool timedOut = false, canceled = false;
    ULONGLONG deadline = GetTickCount64() + 120000; // 解包硬超时 120s
    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 200);
        if (wait == WAIT_OBJECT_0) break;
        if (cancelFlag && *cancelFlag) { canceled = true; break; }
        if (GetTickCount64() > deadline) { timedOut = true; break; }
    }
    if (canceled || timedOut) {
        TerminateProcess(pi.hProcess, 0xFFFFFFFF);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD exitCode = 0xFFFFFFFF;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (canceled) return fail(L"已取消");
    if (timedOut) return fail(L"解包超时（120s），已中止");
    if (exitCode != 0) {
        std::wstring tail = Utf8ToWide(output);
        if (tail.size() > 400) tail = tail.substr(tail.size() - 400);
        return fail(L"tar 解包失败（退出码 " + std::to_wstring(exitCode) + L"）：" + tail);
    }
    return true;
}

// ================================================================ 解出后复核

bool VerifyExtractedDir(const std::wstring& dir, std::wstring* err) {
    unsigned long long total = 0;
    size_t files = 0;
    std::vector<std::wstring> stack{dir};
    while (!stack.empty()) {
        std::wstring cur = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE it = FindFirstFileW((cur + L"\\*").c_str(), &fd);
        if (it == INVALID_HANDLE_VALUE) {
            if (err) *err = L"遍历解出目录失败";
            return false;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = cur + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                FindClose(it);
                if (err) *err = L"解出目录含符号链接/重解析点（拒绝）：" + child;
                return false;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(std::move(child));
            } else {
                unsigned long long size =
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (size > kMaxFileUncompressed) {
                    FindClose(it);
                    if (err) *err = L"解出单文件超过 64MB：" + child;
                    return false;
                }
                total += size;
                if (total > kMaxTotalUncompressed) {
                    FindClose(it);
                    if (err) *err = L"解出总大小超过 256MB（解压炸弹防护）";
                    return false;
                }
                if (++files > kMaxZipEntries) {
                    FindClose(it);
                    if (err) *err = L"解出文件数超过 2048 上限";
                    return false;
                }
            }
        } while (FindNextFileW(it, &fd));
        FindClose(it);
    }
    return true;
}

} // namespace updater
} // namespace lingxi
