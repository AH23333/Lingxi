#pragma once
//
// 细胞词库在线下载导入（纯逻辑层）——cellimport.cpp 的 ImportCellPackFromUrl
// 全部判定走这里，engine_test 直接单测。无网络/无磁盘依赖（header-only）。
//
// 安全模型（与更新链同口径，docs/update-design-2026-11-03 T1/T2）：
//   URL 仅 https + .lxcell 后缀 + 无 userinfo；域名白名单由下载层
//   （updater::DownloadToFile → IsUrlAllowed，GitHub 精确匹配）二次把关——
//   本层是形状闸，白名单是出网闸，两层独立。
//   完整性锚：显式 sha256（64 hex）或同 origin 侧车 <url>.sha256；两者皆无
//   → 拒绝安装（fail-closed，完整性无锚的包不进引擎目录）。
//
#include <cstdint>
#include <string>

namespace lingxi {

// 下载 URL 形闸：https、host 非空、无 '@'（userinfo 走私面）、路径以 .lxcell 结尾。
inline bool ValidateCellUrl(const std::wstring& url)
{
    if (url.size() < 8 + 1 + 7) return false; // "https://"+"x"+"a.lxcell" 下界
    if (url.compare(0, 8, L"https://") != 0) return false;
    if (url.find(L'@') != std::wstring::npos) return false;
    size_t hostEnd = url.find(L'/', 8);
    std::wstring host = url.substr(8, hostEnd == std::wstring::npos
                                        ? std::wstring::npos : hostEnd - 8);
    if (host.empty()) return false;
    if (host.find(L':') != std::wstring::npos)
        host = host.substr(0, host.find(L':')); // 端口剥离后再查空（"https://:443/x"）
    if (host.empty()) return false;
    // 路径须以 .lxcell 结尾（忽略大小写；查询串/片段不许——更新链同口径不吃）
    std::wstring tail = url.size() >= 7 ? url.substr(url.size() - 7) : L"";
    for (auto& ch : tail) if (ch >= L'A' && ch <= L'Z') ch += 32;
    return tail == L".lxcell";
}

// sha256 十六进制：空串合法（语义=走侧车）；非空必须恰好 64 位 hex。
inline bool ValidateSha256Hex(const std::wstring& s)
{
    if (s.empty()) return true;
    if (s.size() != 64) return false;
    for (wchar_t ch : s)
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
              (ch >= L'A' && ch <= L'F')))
            return false;
    return true;
}

// 侧车 URL：原样追加 .sha256（同 origin，下载层白名单同源覆盖）。
inline std::wstring SidecarUrl(const std::wstring& url) { return url + L".sha256"; }

// 侧车内容取哈希：首个空白分隔 token，小写归一；不是 64 位 hex → 空（不可用）。
inline std::wstring ExtractSha256FromSidecar(const std::wstring& text)
{
    size_t b = text.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = text.find_first_of(L" \t\r\n", b);
    std::wstring tok = text.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
    for (auto& ch : tok) if (ch >= L'A' && ch <= L'F') ch += 32;
    return ValidateSha256Hex(tok) && !tok.empty() ? tok : L"";
}

} // namespace lingxi
