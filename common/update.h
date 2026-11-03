#pragma once
//
// ===== 自助更新：检查侧（批次 2）=====
// 设计文档：docs/update-design-2026-11-03.md（威胁模型 T1-T12、发版契约 §2、状态机 §5）。
// 本头当前覆盖：语义化版本比较、GitHub releases/latest 拉取与解析、契约资产选择、
// 检查状态机。下载/校验/解压（批次 3）与替换/自验/回滚（批次 4）在 update_apply.h。
//
// 依赖：仅 WinHTTP + 标准库（零新增第三方库）。消费方：LingxiUI（批次 5 接线）、
// tools/update_test 工装（单测）。
//
#include <windows.h>

#include <string>
#include <vector>

namespace lingxi {
namespace updater {

// ===== 语义化版本（设计 §3）=====
struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

// 解析 "v1.2.3" / "1.0"（缺段补 0，容忍首尾空白与大小写 v）。
// 拒绝：非数字段、空段、负号、超过 3 段、预发布/构建后缀（本期一律不更）。
bool ParseSemVer(const std::wstring& text, SemVer* out);

// 数值三段比较（禁止字符串比较）：a<b → -1，a==b → 0，a>b → 1。
int CompareSemVer(const SemVer& a, const SemVer& b);

// 本机版本（来自真源 common/version.h）。
SemVer LocalVersion();

// ===== Release 信息 =====
struct ReleaseAsset {
    std::wstring name;
    std::wstring downloadUrl; // browser_download_url
    long long size = -1;      // 声明的字节数，-1=缺失
};

struct ReleaseInfo {
    std::wstring tag;         // tag_name 原文（含 v 前缀）
    std::wstring notes;       // body（release notes，UTF-8 已转宽字符）
    std::vector<ReleaseAsset> assets;
};

// 解析 releases/latest 响应 JSON（纯函数，无网络，单测直接打）。
// 只取 tag_name / body / assets[].{name,browser_download_url,size}，未知字段忽略。
bool ParseReleaseJson(const std::string& jsonUtf8, ReleaseInfo* out, std::wstring* err);

// ===== 契约资产选择（设计 §2：缺任一 → 不可用，绝不降级直装）=====
struct SelectedAssets {
    ReleaseAsset x64;
    ReleaseAsset x86;
    ReleaseAsset sums;
};
bool SelectAssets(const ReleaseInfo& rel, SelectedAssets* out, std::wstring* reason);

// ===== 域名白名单（威胁模型 T2）=====
// https + host 精确匹配白名单（api.github.com / github.com / objects.githubusercontent.com）。
// 子域伪装（api.github.com.evil.com）必须判 false。
bool IsUrlAllowed(const std::wstring& url);

// ===== HTTP（WinHTTP，禁自动重定向，逐跳复核白名单；超时/可取消/体积上限）=====
struct HttpReply {
    DWORD status = 0; // HTTP 状态码；0=未收到响应（网络层失败）
    std::string body;
    std::wstring err; // 网络层错误文案（中文，可直接上 UI）
};
// maxBytes 超限、cancelFlag 非 0、超时都返回 err；重定向出白名单同样 err。
HttpReply HttpGet(const std::wstring& url, size_t maxBytes, const volatile LONG* cancelFlag);

// ===== SHA256SUMS 解析（校验在批次 3 做，解析纯函数先行、先测）=====
// 格式：<64位小写hex>  <name>（coreutils 兼容，双空格或 " *"）。返回 name→hex(小写)。
// 非法行（hex 长度/字符不对）→ false。
bool ParseSha256Sums(const std::string& text, std::vector<std::pair<std::wstring, std::string>>* out,
                     std::wstring* err);

// ===== 状态机（设计 §5）=====
enum class State {
    NotConfigured,    // UpdateRepo 为空：正常态，非错误
    Idle,             // 未检查过
    Checking,         // 检查中（UI 态，由调用方持有）
    NotPublished,     // 404 / 无 release：正常态，不弹窗不写错误
    UpToDate,         // 已是最新
    NewVersion,       // 发现新版（附版本与 notes）
    Incomplete,       // 发布包不完整（缺资产/名字不符）：不可用
    Downloading,      // 批次 3
    Verifying,        // 批次 3
    Staged,           // 批次 3：校验+解压完成，待替换
    Replacing,        // 批次 4
    VerifyingInstall, // 批次 4：替换后自验
    AwaitingRestart,  // 成功：等待重启宿主生效
    RollingBack,      // 批次 4
    Failed,           // 失败（err 附原因，可重试）
};

// 中文文案（UI 显示 + 测试钉死）。
const wchar_t* StateText(State s);

// 纯函数：给定 HTTP 响应与本机版本，产出检查结论状态机一步（单测不打网络）。
// 返回 NotPublished / UpToDate / NewVersion / Incomplete / Failed 之一。
State ProcessLatestReply(const HttpReply& reply, const SemVer& local,
                         ReleaseInfo* rel, SemVer* remote, std::wstring* err);

// 完整检查流：组 URL → HttpGet → ProcessLatestReply。
// repo 为空 → NotConfigured；repo 格式非法 → Failed。cancelFlag 可空。
State CheckLatest(const std::wstring& repo, const volatile LONG* cancelFlag,
                  ReleaseInfo* rel, SemVer* remote, std::wstring* err);

// UTF-8 → 宽字符（内部用，导出供测试/批次 5 复用）。
std::wstring Utf8ToWide(const std::string& s);

} // namespace updater
} // namespace lingxi
