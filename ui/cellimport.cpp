// cellimport.cpp - A 细胞词库导入适配层（接口与线程约束见 cellimport.h）
#include "cellimport.h"
#include "cellfetch_logic.h"     // 在线下载：URL/哈希/侧车纯逻辑闸
#include "../common/update_fetch.h" // DownloadToFile / Sha256File（更新链同套安全件）
#include "engine/predictor.h" // ParseLxcell / LxcellParseResult

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "mgmt_logic.h"            // N8b：CellQuotaEffective / SplitIds / ToLowerW
#include "../common/settings.h"    // N8b：停用表（LoadSettings().cellDisabled）

namespace lingxi {

namespace {

// 与引擎侧同值（裁决④单包限额；引擎私有常量不跨层暴露，两处数值由
// engine_test CELLa-12/CELLc 共同钉住，改一处必红）。
constexpr size_t kMaxImportEntries = 200000;
constexpr DWORD kMaxImportFileBytes = 64u * 1024u * 1024u; // 巨文件防御

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool ReadAllW(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 ||
        li.QuadPart > (LONGLONG)kMaxImportFileBytes)
    {
        CloseHandle(h);
        out.clear();
        return li.QuadPart > (LONGLONG)kMaxImportFileBytes; // 超限=失败且非"不存在"
    }
    out.resize((size_t)li.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, NULL);
    CloseHandle(h);
    if (!ok || got != (DWORD)out.size()) { out.clear(); return false; }
    return true;
}

bool WriteAllW(const std::wstring& path, const std::string& content)
{
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, NULL);
    if (ok && written == (DWORD)content.size()) ok = FlushFileBuffers(h); // 批8：刷盘失败按写失败
    CloseHandle(h);
    if (!ok || written != (DWORD)content.size()) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// 深蓝纯文本产物宽容回退解析（§4：列式未实测——按 token 分类而非按列位取：
// 含非 ASCII 字节的 token=词，纯 a-z 字母 token=音节，纯数字 token=词频(忽略)）。
// 跳过计数/跳过率>10%/单包限额与 ParseLxcell 同规则。
bool ParsePlainDict(const std::string& raw, const std::unordered_set<std::string>& sy,
                    size_t maxEntries, LxcellParseResult& r)
{
    std::string body = raw;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF &&
        (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body = body.substr(3);
    size_t pos = 0, lineNo = 0;
    while (pos <= body.size())
    {
        size_t nl = body.find('\n', pos);
        std::string line = (nl == std::string::npos) ? body.substr(pos)
                                                     : body.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';') continue;

        std::string word;
        std::vector<std::string> sylls;
        bool bad = false, hasFreq = false;
        size_t p = 0;
        while (p < line.size())
        {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
            size_t s = p;
            while (p < line.size() && line[p] != ' ' && line[p] != '\t') p++;
            if (s == p) continue;
            std::string tok = line.substr(s, p - s);
            bool nonAscii = false, alpha = true, digit = true;
            for (char ch : tok)
            {
                unsigned char u = (unsigned char)ch;
                if (u >= 0x80) nonAscii = true;
                else if (!(u >= 'a' && u <= 'z') && !(u >= 'A' && u <= 'Z')) alpha = false;
                if (!(u >= '0' && u <= '9')) digit = false;
            }
            if (nonAscii)
            {
                if (!word.empty()) { bad = true; break; } // 两个 CJK 段=歧义行
                word = tok;
            }
            else if (alpha)
            {
                std::string ns;
                ns.reserve(tok.size());
                for (char ch : tok) ns.push_back(char(std::tolower((unsigned char)ch)));
                if (!sy.count(ns)) { bad = true; break; }
                sylls.push_back(std::move(ns));
            }
            else if (digit) hasFreq = true; // 词频列忽略（任意列位）
            else { bad = true; break; }
        }
        if (bad || word.empty() || sylls.empty())
        {
            r.skipped++;
            if (r.skippedLines.size() < 5) r.skippedLines.push_back(lineNo);
            continue;
        }
        (void)hasFreq;
        if (maxEntries > 0 && r.words.size() >= maxEntries)
        {
            r.fatal = true;
            r.overLimit = true;
            r.reason = L"超过单包词条上限（" + std::to_wstring(maxEntries) + L"）";
            return true;
        }
        LxcellWord w;
        w.word = ToWide(word);
        w.sylls = std::move(sylls);
        r.words.push_back(std::move(w));
    }
    size_t entryLines = r.words.size() + r.skipped;
    if (entryLines > 0 && r.skipped * 10 > entryLines)
    {
        r.fatal = true;
        r.reason = L"跳过率超过 10%（" + std::to_wstring(r.skipped) + L"/" +
                   std::to_wstring(entryLines) + L"）——可能不是纯文本词库";
        return true;
    }
    return !r.words.empty(); // 0 词条的"纯文本"视为识别失败（回退误判保护）
}

std::string StemOf(const std::wstring& path)
{
    size_t s = path.find_last_of(L"\\/");
    std::wstring name = (s == std::wstring::npos) ? path : path.substr(s + 1);
    size_t d = name.find_last_of(L'.');
    if (d != std::wstring::npos && d != 0) name = name.substr(0, d);
    return ToUtf8(name);
}

// 轻量扫包：只取 id 头 + 词条行数（含 tab 的行），与 cellwnd ScanPacks 同口径。
// N8b 生效配额模拟用（导入线程，包文件全量 IO 可接受）。
struct PackLite { std::wstring id; size_t entries = 0; };
std::vector<PackLite> ScanPackLites(const std::wstring& dir)
{
    std::vector<PackLite> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.lxcell").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fn = fd.cFileName;
        std::string raw;
        if (!ReadAllW(dir + L"\\" + fn, raw) || raw.empty()) continue;
        PackLite pl;
        bool inHeader = true;
        size_t pos = 0;
        while (pos <= raw.size())
        {
            size_t nl = raw.find('\n', pos);
            std::string line = (nl == std::string::npos) ? raw.substr(pos)
                                                         : raw.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? raw.size() + 1 : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == ';') continue;
            if (line.rfind("LXCELL", 0) == 0) continue;
            if (inHeader && line.find('\t') == std::string::npos &&
                (line.rfind("name=", 0) == 0 || line.rfind("id=", 0) == 0 ||
                 line.rfind("weight=", 0) == 0))
            {
                if (line.rfind("id=", 0) == 0) pl.id = ToWide(line.substr(3));
                continue;
            }
            inHeader = false;
            if (line.find('\t') != std::string::npos) pl.entries++;
        }
        if (pl.id.empty() && fn.size() > 7) pl.id = fn.substr(0, fn.size() - 7); // 去 .lxcell 兜底
        out.push_back(std::move(pl));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

} // namespace

// N8a：归一化丢信息即追加 FNV-1a 哈希后缀（纯 CJK stem 不再全归一成 "pack"）。
// 无信息丢失（纯小写 ascii/数字/- 且首尾非 '-'）保持原样——存量包 id 不变。
std::string MakeIdFromStem(const std::string& stem)
{
    std::string id;
    for (char ch : stem)
    {
        char lc = char(std::tolower((unsigned char)ch));
        if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9') || lc == '-') id.push_back(lc);
        else id.push_back('-'); // 非 id 字符（含 CJK 的 UTF-8 字节）统一转 '-'
    }
    while (!id.empty() && id.front() == '-') id.erase(id.begin());
    while (!id.empty() && id.back() == '-') id.pop_back();
    if (id.empty()) id = "pack";
    std::string lower;
    lower.reserve(stem.size());
    for (char ch : stem) lower.push_back(char(std::tolower((unsigned char)ch)));
    if (id != lower) // 归一化丢失信息（替换/剥离/兜底）→ 哈希后缀去重
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : stem) { h ^= c; h *= 16777619u; }
        char suf[16];
        sprintf_s(suf, "-%08x", h);
        id += suf;
    }
    return id;
}

namespace {

std::string SerializeLxcell(const LxcellParseResult& r)
{
    std::string out = "LXCELL 1\nname=" + r.name + "\nid=" + r.id +
                      "\nweight=" + std::to_string(r.weight) + "\n";
    out.reserve(out.size() + r.words.size() * 24);
    for (const auto& w : r.words)
    {
        out += ToUtf8(w.word);
        out += '\t';
        for (size_t i = 0; i < w.sylls.size(); i++)
        {
            if (i) out += ' ';
            out += w.sylls[i];
        }
        out += '\n';
    }
    return out;
}

} // namespace

std::wstring CellPackDirW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf) + L"\\LingxiIme\\cell";
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exe, MAX_PATH))
    {
        std::wstring base = exe;
        size_t s = base.find_last_of(L"\\/");
        if (s != std::wstring::npos) base = base.substr(0, s);
        return base + L"\\cell";
    }
    return L"cell";
}

CellImportResult ImportCellPackSync(const std::wstring& srcPath,
                                    const std::unordered_set<std::string>& validSylls,
                                    bool overwrite,
                                    const std::wstring& fallbackStem)
{
    CellImportResult res;
    std::string raw;
    if (!ReadAllW(srcPath, raw) || raw.empty())
    {
        res.message = L"读取源文件失败或超过 64MB 上限";
        return res;
    }
    // 严格 UTF-8 预检与 ParseLxcell 同口径；回退解析同样只收 UTF-8
    LxcellParseResult p = ParseLxcell(raw, validSylls, kMaxImportEntries);
    bool converted = false;
    if (p.fatal && !p.overLimit && p.reason.find(L"魔数") != std::wstring::npos)
    {
        LxcellParseResult q;
        if (ParsePlainDict(raw, validSylls, kMaxImportEntries, q))
        {
            // 显示名/id 来源：调用方给的 stem 优先（在线导入=URL 基名），
            // 空则沿用源文件名——staging 临时名（lingxi-cell-dl-*）不外泄。
            std::wstring nameStem = fallbackStem.empty() ? srcPath : fallbackStem;
            std::string stem = ToUtf8(nameStem);
            q.name = stem;                    // 显示名=stem（UTF-8 原样）
            q.id = MakeIdFromStem(stem);      // id=stem 净化（小写/数字/-）
            p = std::move(q);
            converted = true;
        }
    }
    if (p.fatal)
    {
        res.message = p.reason;
        return res;
    }
    res.id = p.id;
    res.imported = p.words.size();
    res.skipped = p.skipped;

    // 目录由导入侧创建（引擎加载侧绝不创建，保证无目录 no-op）；父级一并补齐
    std::wstring dir = CellPackDirW();
    size_t up = dir.find_last_of(L"\\/");
    if (up != std::wstring::npos) CreateDirectoryW(dir.substr(0, up).c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);

    std::wstring target = dir + L"\\" + ToWide(p.id) + L".lxcell";
    // 同名 id 处理：非覆盖=拒绝；覆盖=rename-aside（#89：被占用件改名而非删除）
    std::wstring aside;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (!overwrite)
        {
            res.message = L"包 id 已存在：" + ToWide(p.id) + L"（选择覆盖或先删除）";
            return res;
        }
        aside = target + L"." + std::to_wstring(GetTickCount()) + L".prev";
        if (!MoveFileExW(target.c_str(), aside.c_str(), 0))
        {
            res.message = L"旧包文件无法改名让位（可能被占用）";
            return res;
        }
    }
    auto rollback = [&]() {
        DeleteFileW(target.c_str());
        if (!aside.empty()) MoveFileExW(aside.c_str(), target.c_str(), 0);
    };

    std::string content = SerializeLxcell(p);
    if (!WriteAllW(target, content))
    {
        rollback();
        res.message = L"落盘失败（tmp+rename 未成）";
        return res;
    }

    // A6 独立复核：重读落盘件、重新解析、逐项比对（禁止只看写盘返回值）
    std::string back;
    LxcellParseResult v = ReadAllW(target, back)
        ? ParseLxcell(back, validSylls, kMaxImportEntries)
        : LxcellParseResult{};
    if (v.fatal || v.id != p.id || v.words.size() != p.words.size())
    {
        rollback();
        res.message = L"导入未确认：落盘复核不一致（内存 " + std::to_wstring(p.words.size()) +
                      L" / 磁盘 " + std::to_wstring(v.fatal ? 0 : v.words.size()) + L"）";
        return res;
    }

    if (!aside.empty()) DeleteFileW(aside.c_str());
    res.ok = true;
    res.message = L"成功 " + std::to_wstring(p.words.size()) + L" 跳过 " +
                  std::to_wstring(p.skipped) + L" 条";
    if (!p.skippedLines.empty())
    {
        res.message += L"（行号";
        for (size_t i = 0; i < p.skippedLines.size(); i++)
        {
            res.message += (i ? L"," : L"：") + std::to_wstring(p.skippedLines[i]);
        }
        res.message += L"）";
    }
    if (converted) res.message += L"（深蓝纯文本产物已规范化为 .lxcell）";

    // N8b：生效口径——镜像引擎 RebuildCellPacks（剔停用→id 升序→合计上限
    // 整包跳），模拟本包是否真生效；不生效在文案明示，杜绝"说成功但配额拒"。
    {
        constexpr size_t kMaxCellTotal = 500000; // 与引擎 kMaxCellTotalEntries 同值
                                                 //（engine_test CELLd 钉两侧一致）
        Settings qst = LoadSettings();
        std::unordered_set<std::wstring> dis;
        for (const auto& d : mgmt::SplitIds(qst.cellDisabled))
        {
            std::wstring l = mgmt::ToLowerW(d);
            if (!l.empty()) dis.insert(l);
        }
        std::vector<std::pair<std::wstring, size_t>> enabled;
        std::unordered_set<std::wstring> seenIds;
        for (const auto& pl : ScanPackLites(dir))
        {
            std::wstring lid = mgmt::ToLowerW(pl.id);
            if (dis.count(lid) || seenIds.count(lid)) continue; // 停用/重复 id 先到者胜
            seenIds.insert(lid);
            enabled.push_back({ pl.id, pl.entries });
        }
        if (mgmt::CellQuotaEffective(enabled, ToWide(p.id), kMaxCellTotal) == 0)
            res.message += L"（注意：启用合计超 500000 条上限，本包暂不生效——"
                           L"停用或删除其他包后下一次输入自动生效）";
    }
    return res;
}

namespace {

// 在线下载：单包上限（文本词库，远大于常规包；下载层另受 kMaxDownloadBytes 兜底）
constexpr size_t kMaxCellDownloadBytes = 8ull * 1024 * 1024;
constexpr size_t kMaxSidecarBytes = 4096; // 侧车 .sha256 只有一行哈希

bool RealCellDownload(const std::wstring& url, const std::wstring& destPath,
                      size_t maxBytes, std::wstring* err)
{
    return updater::DownloadToFile(url, destPath, maxBytes, nullptr, nullptr, nullptr, err);
}

std::wstring CellDlStaging(const wchar_t* suffix)
{
    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[MAX_PATH];
    _snwprintf_s(name, _TRUNCATE, L"%lslingxi-cell-dl-%lu-%llu%ls", tmp,
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64(), suffix);
    return name;
}

} // namespace

// 在线下载导入（声明见 cellimport.h）：形闸 → staging → 完整性锚 → 本地同管导入
CellImportResult ImportCellPackFromUrl(const std::wstring& url,
                                       const std::wstring& sha256HexOrEmpty,
                                       const std::unordered_set<std::string>& validSylls,
                                       bool overwrite,
                                       CellDownloadFn downloader)
{
    CellImportResult res;
    if (!ValidateCellUrl(url))
    {
        res.message = L"URL 不合格：须 https 且以 .lxcell 结尾、不含 @（域名白名单另由下载层把关）";
        return res;
    }
    // 深蓝纯文本回退的显示名/id 来源：URL 末段基名（形闸保证路径以 .lxcell 结尾、
    // 无查询/片段）。仅作显示名与 id 派生输入，不落盘为文件名；非 id 字符由
    // MakeIdFromStem 净化，%xx 类 URL 编码保持原样（可读即可）。
    // 边界：无路径形态（"https://x.lxcell"，host 即基名）find_last_of 只命中
    // scheme 的斜杠——末段若含 "://" 则整体不是基名，退回 "online-pack"。
    std::wstring urlBase;
    {
        size_t slash = url.find_last_of(L'/');
        std::wstring cand = (slash == std::wstring::npos) ? L"" : url.substr(slash + 1);
        if (cand.find(L"://") == std::wstring::npos && !cand.empty()) urlBase = cand;
        if (urlBase.empty()) urlBase = L"online-pack";
    }
    if (urlBase.size() > 96) urlBase = urlBase.substr(0, 96);
    if (!ValidateSha256Hex(sha256HexOrEmpty))
    {
        res.message = L"SHA256 须为 64 位十六进制（或留空自动取侧车 .sha256）";
        return res;
    }
    CellDownloadFn dl = downloader ? downloader : &RealCellDownload;
    const std::wstring staging = CellDlStaging(L".lxcell");
    std::wstring err;
    auto cleanup = [&]() {
        DeleteFileW(staging.c_str());
        DeleteFileW((staging + L".sha256").c_str());
    };
    if (!dl(url, staging, kMaxCellDownloadBytes, &err))
    {
        cleanup();
        res.message = L"下载失败：" + err;
        return res;
    }
    // 完整性锚：显式 hex 优先；缺省走同 origin 侧车（fail-closed）
    std::wstring expected = sha256HexOrEmpty;
    for (auto& ch : expected) if (ch >= L'A' && ch <= L'F') ch += 32;
    if (expected.empty())
    {
        const std::wstring scPath = staging + L".sha256";
        if (!dl(SidecarUrl(url), scPath, kMaxSidecarBytes, &err))
        {
            cleanup();
            res.message = L"未给 SHA256 且侧车 .sha256 取不到（" + err +
                          L"）：拒绝安装——完整性无锚";
            return res;
        }
        std::string sc;
        if (!ReadAllW(scPath, sc))
        {
            cleanup();
            res.message = L"侧车读取失败：拒绝安装";
            return res;
        }
        expected = ExtractSha256FromSidecar(ToWide(sc));
        if (expected.empty())
        {
            cleanup();
            res.message = L"侧车 .sha256 内容不合格：拒绝安装";
            return res;
        }
    }
    std::string actual;
    if (!updater::Sha256File(staging, &actual, &err))
    {
        cleanup();
        res.message = L"哈希计算失败：" + err;
        return res;
    }
    if (ToWide(actual) != expected)
    {
        cleanup();
        res.message = L"SHA256 不符：下载件与锚不一致，已整包丢弃";
        return res;
    }
    res = ImportCellPackSync(staging, validSylls, overwrite, urlBase);
    if (res.ok) res.message = L"在线导入：" + res.message;
    cleanup();
    return res;
}

// N7：rename-aside 的 .del 死件清理（DoDelete 改名让位后无删除路径的根治）。
// 仍被占用（引擎正读）的 .del 删除失败属正常——下次开窗/操作后再试，调用方记日志。
size_t CleanupCellDelResidue(size_t* failed)
{
    if (failed) *failed = 0;
    size_t deleted = 0;
    std::wstring dir = CellPackDirW();
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.del").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) deleted++;
        else if (failed) (*failed)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return deleted;
}

} // namespace lingxi
