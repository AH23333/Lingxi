// skinstore.cpp - 在线皮肤：目录扫描 + 运行时注册表 + URL 导入管线（接口见 skinstore.h）
#include "skinstore.h"
#include "skinformat_logic.h"      // ParseLxskin / ValidateSkinUrl / SerializeLxskin
#include "cellfetch_logic.h"       // ValidateSha256Hex / SidecarUrl / ExtractSha256FromSidecar
#include "../common/update_fetch.h" // DownloadToFile / Sha256File（更新链同套安全件）

#include <algorithm>

namespace lingxi {

namespace {

// 文件 IO（与 cellimport.cpp 匿名件同型：共享删、64MB 闸、tmp+rename+刷盘原子写）
constexpr DWORD kMaxSkinFileBytes = 1024u * 1024u; // 读闸（解析层 64KB 更严，双闸）

bool ReadAllW(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 ||
        li.QuadPart > (LONGLONG)kMaxSkinFileBytes)
    {
        CloseHandle(h);
        out.clear();
        return false;
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
    if (ok && written == (DWORD)content.size()) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != (DWORD)content.size()) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

std::wstring SkinDirW()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf) + L"\\LingxiIme\\skins";
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exe, MAX_PATH))
    {
        std::wstring base = exe;
        size_t s = base.find_last_of(L"\\/");
        if (s != std::wstring::npos) base = base.substr(0, s);
        return base + L"\\skins";
    }
    return L"skins";
}

size_t ScanSkinDir(const std::wstring& dir, std::vector<DlSkin>* out, size_t* skipped)
{
    if (skipped) *skipped = 0;
    out->clear();
    struct Cand { std::wstring file; DlSkin s; };
    std::vector<Cand> valid;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.lxskin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string raw;
        if (!ReadAllW(dir + L"\\" + fd.cFileName, raw))
        {
            if (skipped) (*skipped)++;
            continue;
        }
        LxskinDef p = ParseLxskin(raw);
        if (!p.ok)
        {
            if (skipped) (*skipped)++;
            continue;
        }
        DlSkin s;
        s.id = p.id;
        s.name = p.name;
        s.def.accent = p.accent;
        s.def.hoverLight = p.hover;
        valid.push_back({ fd.cFileName, std::move(s) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // (id, 文件名) 升序：重复 id 文件名小者胜（确定性，与 FindFirstFile 枚举序无关）
    std::sort(valid.begin(), valid.end(), [](const Cand& a, const Cand& b) {
        if (a.s.id != b.s.id) return a.s.id < b.s.id;
        return a.file < b.file;
    });
    std::wstring lastId;
    for (auto& c : valid)
    {
        if (c.s.id == lastId) { if (skipped) (*skipped)++; continue; } // 重复 id 先到者胜
        lastId = c.s.id;
        if (out->size() >= kMaxDlSkins) { if (skipped) (*skipped)++; continue; }
        out->push_back(std::move(c.s));
    }
    return out->size();
}

// ---- 运行时注册表 ----
namespace {

struct SkinReg
{
    CRITICAL_SECTION cs;
    std::vector<DlSkin> skins;
    SkinReg() { InitializeCriticalSection(&cs); }
    // 刻意不析构 CS（退出竞态同 CandWindow::Shutdown 裁决：泄漏无害，删锁有崩）
};

SkinReg& Reg()
{
    static SkinReg r; // magic static：首次调用线程安全初始化
    return r;
}

} // namespace

void UiReloadSkins()
{
    std::vector<DlSkin> fresh;
    ScanSkinDir(SkinDirW(), &fresh);
    SkinReg& r = Reg();
    EnterCriticalSection(&r.cs);
    r.skins = std::move(fresh);
    LeaveCriticalSection(&r.cs);
}

size_t UiDlSkinCount()
{
    SkinReg& r = Reg();
    EnterCriticalSection(&r.cs);
    size_t n = r.skins.size();
    LeaveCriticalSection(&r.cs);
    return n;
}

bool UiDlSkinAt(size_t i, DlSkin* out)
{
    SkinReg& r = Reg();
    EnterCriticalSection(&r.cs);
    bool ok = i < r.skins.size();
    if (ok && out) *out = r.skins[i];
    LeaveCriticalSection(&r.cs);
    return ok;
}

bool UiDlSkinById(const std::wstring& id, DlSkin* out)
{
    SkinReg& r = Reg();
    EnterCriticalSection(&r.cs);
    for (const auto& s : r.skins)
    {
        if (s.id == id)
        {
            if (out) *out = s;
            LeaveCriticalSection(&r.cs);
            return true;
        }
    }
    LeaveCriticalSection(&r.cs);
    return false;
}

SkinDef UiCurrentSkin(const Settings& s)
{
    if (!s.skinId.empty())
    {
        DlSkin d;
        if (UiDlSkinById(s.skinId, &d)) return d.def;
    }
    return UiSkin(s.skinIndex); // 内置越界回退 0（SK4）
}

std::wstring UiCurrentSkinName(const Settings& s)
{
    if (!s.skinId.empty())
    {
        DlSkin d;
        if (UiDlSkinById(s.skinId, &d)) return d.name;
        return s.skinId + L"（已删除，回退内置）";
    }
    return kSkinNames[s.skinIndex < kSkinCount ? s.skinIndex : 0];
}

// ---- 在线导入 ----
namespace {

constexpr size_t kMaxSkinDownloadBytes = 256u * 1024u; // 皮肤包正常 <1KB
constexpr size_t kMaxSidecarBytes = 4096;              // 侧车 .sha256 只有一行哈希

bool RealSkinDownload(const std::wstring& url, const std::wstring& destPath,
                      size_t maxBytes, std::wstring* err)
{
    return updater::DownloadToFile(url, destPath, maxBytes, nullptr, nullptr, nullptr, err);
}

std::wstring SkinDlStaging(const wchar_t* suffix)
{
    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[MAX_PATH];
    _snwprintf_s(name, _TRUNCATE, L"%lslingxi-skin-dl-%lu-%llu%ls", tmp,
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64(), suffix);
    return name;
}

} // namespace

SkinImportResult ImportSkinFromUrl(const std::wstring& url,
                                   const std::wstring& sha256HexOrEmpty,
                                   SkinDownloadFn downloader)
{
    SkinImportResult res;
    if (!ValidateSkinUrl(url))
    {
        res.message = L"URL 不合格：须 https 且以 .lxskin 结尾、不含 @（域名白名单另由下载层把关）";
        return res;
    }
    if (!ValidateSha256Hex(sha256HexOrEmpty))
    {
        res.message = L"SHA256 须为 64 位十六进制（或留空自动取侧车 .sha256）";
        return res;
    }
    SkinDownloadFn dl = downloader ? downloader : &RealSkinDownload;
    const std::wstring staging = SkinDlStaging(L".lxskin");
    std::wstring err;
    auto cleanup = [&]() {
        DeleteFileW(staging.c_str());
        DeleteFileW((staging + L".sha256").c_str());
    };
    if (!dl(url, staging, kMaxSkinDownloadBytes, &err))
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
        expected = ExtractSha256FromSidecar(SkinWideFromU8(sc));
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
    if (SkinWideFromU8(actual) != expected)
    {
        cleanup();
        res.message = L"SHA256 不符：下载件与锚不一致，已整包丢弃";
        return res;
    }
    // 解析验证（staging 原件）
    std::string raw;
    if (!ReadAllW(staging, raw))
    {
        cleanup();
        res.message = L"下载件读取失败";
        return res;
    }
    LxskinDef def = ParseLxskin(raw);
    if (!def.ok)
    {
        cleanup();
        res.message = L"皮肤包不合格：" + def.reason;
        return res;
    }
    // 落盘（规范化重写；目录在此创建——扫描侧绝不创建）
    std::wstring dir = SkinDirW();
    size_t up = dir.find_last_of(L"\\/");
    if (up != std::wstring::npos) CreateDirectoryW(dir.substr(0, up).c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring target = dir + L"\\" + def.id + L".lxskin";
    if (!WriteAllW(target, SerializeLxskin(def)))
    {
        cleanup();
        res.message = L"落盘失败（目录不可写？）";
        return res;
    }
    // 独立复核（A6 同纪律）：重读落盘件再解析，逐项比对——禁止只看写盘返回值报成功
    std::string back;
    LxskinDef chk;
    if (!ReadAllW(target, back) || !(chk = ParseLxskin(back)).ok ||
        chk.id != def.id || chk.name != def.name ||
        chk.accent != def.accent || chk.hover != def.hover)
    {
        DeleteFileW(target.c_str());
        cleanup();
        res.message = L"导入未确认：落盘复核不一致，已回退";
        return res;
    }
    res.ok = true;
    res.id = def.id;
    res.message = L"在线导入：" + def.name + L"（" + def.id + L"）";
    cleanup();
    return res;
}

bool DeleteSkin(const std::wstring& id, std::wstring* err)
{
    if (!ValidateSkinId(id))
    {
        if (err) *err = L"id 不合格";
        return false;
    }
    std::wstring target = SkinDirW() + L"\\" + id + L".lxskin";
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (err) *err = L"皮肤不存在";
        return false;
    }
    if (!DeleteFileW(target.c_str()))
    {
        if (err) *err = L"删除失败 err=" + std::to_wstring(GetLastError());
        return false;
    }
    return true;
}

} // namespace lingxi
