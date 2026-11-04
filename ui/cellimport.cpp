// cellimport.cpp - A 细胞词库导入适配层（接口与线程约束见 cellimport.h）
#include "cellimport.h"
#include "engine/predictor.h" // ParseLxcell / LxcellParseResult

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

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
    return id;
}

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
                                    bool overwrite)
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
            std::string stem = StemOf(srcPath);
            q.name = stem;                    // 显示名=文件名（UTF-8 原样）
            q.id = MakeIdFromStem(stem);      // id=文件名净化（小写/数字/-）
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
