#pragma once
//
// 在线皮肤包 .lxskin（纯逻辑层）——skinstore.cpp 的解析/验证全部走这里，
// engine_test 直接单测。无网络/无磁盘依赖（header-only）。
//
// 包格式（UTF-8 文本，CRLF 容错，';' 起注释行，未知键忽略以前向兼容）：
//   LXSKIN 1
//   name=墨玉
//   id=moyu
//   accent=16,124,16      （必填；白字可读性闸：Rec.601 亮度 ≤160）
//   hover=230,242,230     （可选；缺省=accent 每通道向白混入 7/8，≈内置肤 0.875 配比）
//
// 安全模型与细胞词库在线源同口径（cellfetch_logic.h/cellimport.cpp）：
//   URL 形闸在本头（https/.lxskin/无 userinfo）；域名白名单由下载层
//   （updater::DownloadToFile → IsUrlAllowed）二次把关——本层是形状闸。
//   完整性锚：显式 sha256（64 hex）或同 origin 侧车 <url>.sha256（三件
//   直接复用 cellfetch_logic.h，不重复造）。两者皆无 → 拒绝安装（fail-closed）。
//
#include <windows.h> // COLORREF——与 uistyle.h 同色型
#include <cstdint>
#include <string>

namespace lingxi {

// 包体上限（解析层防御；下载层另有 256KB 闸兜底——皮肤包正常 <1KB）
constexpr size_t kMaxLxskinBytes = 64u * 1024u;
// 已装皮肤数上限（扫描层；防目录被灌爆拖慢启动）
constexpr size_t kMaxDlSkins = 64;

struct LxskinDef
{
    bool ok = false;
    std::wstring id;
    std::wstring name;
    COLORREF accent = 0;
    COLORREF hover = 0;
    bool hoverDerived = false; // true=hover 由 accent 派生（包内未显式给）
    std::wstring reason;       // 失败原因（人读）
};

// ---- UTF-8 ↔ wide（本头自足，不依赖调用方转换件）----
inline std::wstring SkinWideFromU8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string SkinU8FromWide(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

// 下载 URL 形闸：https、host 非空、无 '@'（userinfo 走私面）、路径以 .lxskin 结尾。
//（与 cellfetch_logic.h ValidateCellUrl 同型，后缀不同；查询串/片段不许。）
inline bool ValidateSkinUrl(const std::wstring& url)
{
    if (url.size() < 8 + 1 + 7) return false; // "https://"+"x"+"a.lxskin" 下界
    if (url.compare(0, 8, L"https://") != 0) return false;
    if (url.find(L'@') != std::wstring::npos) return false;
    size_t hostEnd = url.find(L'/', 8);
    std::wstring host = url.substr(8, hostEnd == std::wstring::npos
                                        ? std::wstring::npos : hostEnd - 8);
    if (host.empty()) return false;
    if (host.find(L':') != std::wstring::npos)
        host = host.substr(0, host.find(L':')); // 端口剥离后再查空
    if (host.empty()) return false;
    std::wstring tail = url.size() >= 7 ? url.substr(url.size() - 7) : L"";
    for (auto& ch : tail) if (ch >= L'A' && ch <= L'Z') ch += 32;
    return tail == L".lxskin";
}

// id 闸：小写字母/数字/-，1..32，首尾非 '-'（落盘名 <id>.lxskin，严闸防路径注入）
inline bool ValidateSkinId(const std::wstring& id)
{
    if (id.empty() || id.size() > 32) return false;
    if (id.front() == L'-' || id.back() == L'-') return false;
    for (wchar_t ch : id)
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-'))
            return false;
    return true;
}

// "r,g,b" 十进制（ASCII 域；每通道 0-255，允许空白包围，其余字符即拒）
inline bool ParseColorRgb(const std::string& text, COLORREF* out)
{
    unsigned v[3] = { 0, 0, 0 };
    size_t pos = 0;
    for (int i = 0; i < 3; i++)
    {
        size_t comma = (i < 2) ? text.find(',', pos) : std::string::npos;
        if (i < 2 && comma == std::string::npos) return false;
        std::string part = (comma == std::string::npos)
                               ? text.substr(pos) : text.substr(pos, comma - pos);
        pos = (comma == std::string::npos) ? text.size() + 1 : comma + 1;
        size_t b = part.find_first_not_of(" \t");
        size_t e = part.find_last_not_of(" \t");
        if (b == std::string::npos) return false;
        part = part.substr(b, e - b + 1);
        if (part.empty() || part.size() > 3) return false;
        unsigned n = 0;
        for (char ch : part)
        {
            if (ch < '0' || ch > '9') return false;
            n = n * 10 + (unsigned)(ch - '0');
        }
        if (n > 255) return false;
        v[i] = n;
    }
    *out = RGB(v[0], v[1], v[2]);
    return true;
}

// accent 亮度（Rec.601）。白字可读性闸：内置 7 肤最高 112（金），闸设 160
// —— 放行略亮于内置的肤色，挡住白字不可读的浅肤（fail-closed UX）。
inline int AccentLuminance(COLORREF c)
{
    return (299 * GetRValue(c) + 587 * GetGValue(c) + 114 * GetBValue(c)) / 1000;
}

inline bool AccentReadable(COLORREF c) { return AccentLuminance(c) <= 160; }

// hover 派生：每通道 accent + (255-accent)*7/8（整数截断，与断言同式）
inline COLORREF DeriveHover(COLORREF accent)
{
    auto mix = [](int a) { return a + ((255 - a) * 7) / 8; };
    return RGB(mix(GetRValue(accent)), mix(GetGValue(accent)), mix(GetBValue(accent)));
}

// 解析全包（fail-closed）：魔数/必填缺失/色越界/亮度不过关/id 不合格 → ok=false。
inline LxskinDef ParseLxskin(const std::string& rawUtf8)
{
    LxskinDef d;
    if (rawUtf8.empty() || rawUtf8.size() > kMaxLxskinBytes)
    {
        d.reason = L"包体为空或超过 64KB 上限";
        return d;
    }
    std::string body = rawUtf8;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF &&
        (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body = body.substr(3);

    bool magicOk = false;
    std::string nameU8, idU8, accentU8, hoverU8;
    bool hasName = false, hasId = false, hasAccent = false, hasHover = false;
    size_t pos = 0;
    while (pos <= body.size())
    {
        size_t nl = body.find('\n', pos);
        std::string line = (nl == std::string::npos) ? body.substr(pos)
                                                     : body.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line[0] == ';') continue;
        if (!magicOk)
        {
            if (line != "LXSKIN 1") { d.reason = L"缺 LXSKIN 1 魔数头"; return d; }
            magicOk = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            d.reason = L"存在非 key=value 行";
            return d;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        size_t vb = val.find_first_not_of(" \t");
        val = (vb == std::string::npos) ? "" : val.substr(vb);
        if (key == "name") { nameU8 = val; hasName = true; }
        else if (key == "id") { idU8 = val; hasId = true; }
        else if (key == "accent") { accentU8 = val; hasAccent = true; }
        else if (key == "hover") { hoverU8 = val; hasHover = true; }
        // 未知键忽略（前向兼容）
    }
    if (!magicOk) { d.reason = L"缺 LXSKIN 1 魔数头"; return d; }
    if (!hasName || !hasId || !hasAccent)
    {
        d.reason = L"缺必填项（name/id/accent 不可少）";
        return d;
    }
    d.name = SkinWideFromU8(nameU8);
    if (d.name.empty() || d.name.size() > 64)
    {
        d.reason = L"name 为空或超过 64 字";
        return d;
    }
    for (wchar_t ch : d.name)
        if (ch < 0x20) { d.reason = L"name 含控制字符"; return d; }
    d.id = SkinWideFromU8(idU8);
    if (!ValidateSkinId(d.id))
    {
        d.reason = L"id 不合格（小写字母/数字/-，1..32，首尾非 -）";
        return d;
    }
    if (!ParseColorRgb(accentU8, &d.accent))
    {
        d.reason = L"accent 颜色不合格（须 r,g,b 十进制 0-255）";
        return d;
    }
    if (!AccentReadable(d.accent))
    {
        d.reason = L"accent 亮度过高：白字不可读（闸 160）";
        return d;
    }
    if (hasHover)
    {
        if (!ParseColorRgb(hoverU8, &d.hover))
        {
            d.reason = L"hover 颜色不合格（须 r,g,b 十进制 0-255）";
            return d;
        }
    }
    else
    {
        d.hover = DeriveHover(d.accent);
        d.hoverDerived = true;
    }
    d.ok = true;
    return d;
}

// 规范化重写（导入落盘用）：标准头+四行，剥离注释/未知键；hover 恒显式化
//（派生值固化，重载无歧义）。
inline std::string SerializeLxskin(const LxskinDef& d)
{
    std::string out = "LXSKIN 1\nname=" + SkinU8FromWide(d.name) +
                      "\nid=" + SkinU8FromWide(d.id) + "\n";
    auto col = [](COLORREF c) {
        return std::to_string(GetRValue(c)) + "," + std::to_string(GetGValue(c)) +
               "," + std::to_string(GetBValue(c));
    };
    out += "accent=" + col(d.accent) + "\nhover=" + col(d.hover) + "\n";
    return out;
}

} // namespace lingxi
