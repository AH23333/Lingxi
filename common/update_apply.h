#pragma once
//
// ===== 自助更新：替换 / 自验 / 回滚（批次 4）=====
// 设计文档：docs/update-design-2026-11-03.md §6（流程）、§4 T9/T10/T12（威胁模型）。
// 覆盖：staging 路径校验（T10，提权侧只接受 bin\work\staging\<合法版本>）、
// 双位替换计划（x64+x86 缺一不可）、rename-aside 替换（PITFALLS #89）、
// 独立自验（T12：imecheck + 探针 --version + 落盘 SHA256 比对，不采信步骤返回值）、
// 任一步失败整体回滚（T9：要么全换要么全回滚，不留半替换态）。
//
// 生效边界（§7）：本模块只保证磁盘与注册表是新件；DLL 镜像被宿主持有，
// 生效必须重启宿主/注销——调用方必须在成功后明确提示这一点。
//
// 依赖：仅 Win32 基础 API（CreateProcess/ShellExecute/CNG 复用 update_fetch）。
// 消费方：批次 5 的 LingxiUI.exe --update-apply（提权侧）与设置页按钮（发起侧）；
// tools/update_test 集成测试（假 bin 树，绝不碰真 bin\Debug*）。
//
#include <windows.h>

#include <string>
#include <vector>

#include "update.h"

namespace lingxi {
namespace updater {

// ===== staging 路径校验（T10）=====
// stagingDir 规范化后必须是 <binWorkRoot>\staging\ 的**直接子目录**（大小写不敏感），
// 子目录名必须可 ParseSemVer，且真实存在、是目录。任何一条不满足 → false + 原因。
bool ValidateStagingPath(const std::wstring& stagingDir, const std::wstring& binWorkRoot,
                         SemVer* ver, std::wstring* err);

// 由本进程模块路径推导：<bin>\<任意子目录>\<exe> → binRoot=<bin>、workRoot=<bin>\work。
// UI 侧（批次 5 下载/暂存）与提权侧（--update-apply）必须同口径，防止两侧算出不同根。
bool ComputeBinRootsFromModule(std::wstring* binRoot, std::wstring* workRoot);

// ===== 替换计划（纯函数，可单测）=====
struct FileOp {
    std::wstring rel; // 相对子树路径（正斜杠已归一反斜杠）
    std::wstring src; // staging\x64|x86\<rel>
    std::wstring dst; // binRoot\Debug|Debug32\<rel>
};

// stagingDir\x64\** → binRoot\Debug\**，stagingDir\x86\** → binRoot\Debug32\**。
// 规则：两侧都必须存在且非空（缺 32 位=契约违反，直接拒绝执行——历史教训）；
// 两侧都必须含 ImeCore.dll（重注册与自验依赖）；枚举中拒符号链接/重解析点。
bool BuildReplacePlan(const std::wstring& stagingDir, const std::wstring& binRoot,
                      std::vector<FileOp>* plan, std::wstring* err);

// ===== 落盘自验：逐文件 SHA256（staging ↔ bin 比对，T12 的独立复核部分）=====
bool VerifyInstalledHashes(const std::vector<FileOp>& plan, std::wstring* err);

// ===== 外部动作的真实实现（各自独立可测；集成测试用 hooks 注入桩）=====
// TSF 重注册：x64 regsvr32 + SysWOW64 regsvr32（两侧都要，等价 tools\register.cmd）。
bool ReRegisterTsf(const std::wstring& dll64, const std::wstring& dll32, std::wstring* err);
// imecheck：powershell -File <script>，退出码 0 才算过。
bool RunImecheck(const std::wstring& scriptPath, std::wstring* err);
// 探针：exe --version，捕获 stdout（UTF-16LE/UTF-8 自适应解码），
// 退出码 0 且输出含 versionStr 才算过。
bool RunVersionProbe(const std::wstring& exePath, const std::wstring& versionStr,
                     std::wstring* err);

// ===== 编排 hooks（任一为 nullptr 用对应真实实现）=====
struct ApplyHooks {
    bool (*reRegister)(const std::wstring& dll64, const std::wstring& dll32,
                       std::wstring* err) = nullptr;
    bool (*imecheck)(const std::wstring& scriptPath, std::wstring* err) = nullptr;
    bool (*probe)(const std::wstring& exePath, const std::wstring& versionStr,
                  std::wstring* err) = nullptr;
};

struct ApplyRequest {
    std::wstring binRoot;        // 含 Debug\ 与 Debug32\ 的 bin 根
    std::wstring stagingDir;     // 含 x64\ 与 x86\ 子树（调用方先过 ValidateStagingPath）
    std::wstring versionStr;     // 新版本串（探针匹配用，如 L"1.2.3"）
    std::wstring imecheckScript; // imecheck.ps1 路径；空 = 跳过该自验步骤
    bool doRegister = true;      // false = 跳过 TSF 重注册（仅限演练/测试）
};

// 完整流程：计划 → rename-aside+复制（T9 原子性）→ 重注册 → 自验（imecheck+探针+哈希）
// → 任一步失败整体回滚（删新件、.prev-* 原名改回）。
// 成功后保留最近 2 套 .prev-<ts> 快照（T3），更早的自动清理（失败只记日志）。
// log 可空；非空则逐步追加中文进度（UI 展示）。
bool ApplyUpdate(const ApplyRequest& req, const ApplyHooks& hooks, std::wstring* err,
                 std::wstring* log);

// ===== T3 .prev-* 快照清理 =====
// 扫描 binRoot\Debug 与 Debug32 下 *.prev-*（含同秒撞名的 -<seq> 尾件，同 ts 归一套），
// 按时间戳保留最近 keepSets 套，删除更早的。删除失败（占用等）只记 log 不影响结果。
// 返回成功删除的文件数。
size_t CleanupOldPrevSnapshots(const std::wstring& binRoot, size_t keepSets,
                               std::wstring* log);

// ===== P1-2 staging 内容信任锚（审查 review-2026-11-04-system-integration）=====
// 问题：staging 位于用户可写目录，下载期校验（zip↔SUMS）到 UAC 确认之间存在
// 换包窗口；旧提权侧只验路径合法 + 自验哈希以 staging 自身为锚（换包后两側
// 一起换，检出不了）。
// 方案：发起侧在解压复核后调用 BuildStagingManifest 生成逐文件清单（排序确定序），
// 写到 workRoot 下清单文件，并把**清单文件的 SHA256** 随 UAC 命令行传入提权侧；
// 提权侧先复核清单文件哈希（argv 是唯一不被磁盘篡改锚定的通道），再按清单复核
// staging 全文件，任何一件不符即整体拒绝、零替换。
// 残余风险如实声明：同用户进程若在发起侧进程内执行代码可伪造 argv——该前提已
// 等同整机失守，不在本链威胁模型内（设计文档 T3 同款口径）。
bool BuildStagingManifest(const std::wstring& stagingDir, std::wstring* manifest,
                          std::wstring* err);
bool VerifyStagingAgainstManifest(const std::wstring& stagingDir,
                                  const std::wstring& manifest, std::wstring* err);
// 提权侧一站式：复核清单文件哈希 → 读清单（UTF-8）→ 复核 staging 全文件。
// expectManifestSha256 为 argv 传入的清单文件 SHA256（小写 hex）。
bool VerifyStagingTrusted(const std::wstring& stagingDir, const std::wstring& manifestPath,
                          const std::string& expectManifestSha256, std::wstring* err);

// ===== 一次性 UAC 提权发起（批次 5 UI 调用；本批不做真实 UAC 测试）=====
// ShellExecuteW "runas" selfExe，参数 --update-apply "<stagingDir>" --manifest
// "<manifestPath>" --manifest-sha "<hex>"（T10 + P1-2：提权侧收到后必须自行
// ValidateStagingPath + VerifyStagingTrusted，本函数只负责发起）。
// 用户拒绝 UAC → false + "已取消"。
bool LaunchElevatedApply(const std::wstring& selfExe, const std::wstring& stagingDir,
                         const std::wstring& manifestPath, const std::string& manifestSha256,
                         std::wstring* err);

// ===== T2 pending-rollback 补救（回滚不完整不再是"诚实但无补救"）=====
// 回滚不完整时（新件被宿主加载删不掉/旧件改不回）落标记文件
// <binRoot>\work\pending-rollback.txt（UTF-8 行式：reason=/copied=/prev=<prev>|<dst>）。
std::wstring PendingRollbackMarkerPath(const std::wstring& binRoot);
// LingxiUI 启动自检：无标记 → true（no-op）；有标记 → 按标记补做回滚
//（删新件+旧快照改回；已缺失的件按"上轮部分回滚已完成"判定幂等）。
// 全成 → 删标记返回 true；任一失败 → 保留标记、log 追加明细（不静默）返回 false。
bool TryPendingRollbackSelfHeal(const std::wstring& binRoot, std::wstring* log);

} // namespace updater
} // namespace lingxi
