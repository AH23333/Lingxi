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
// 成功后 bin 目录保留 .prev-<ts> 快照（最近一轮），绝不自动删除。
// log 可空；非空则逐步追加中文进度（UI 展示）。
bool ApplyUpdate(const ApplyRequest& req, const ApplyHooks& hooks, std::wstring* err,
                 std::wstring* log);

// ===== 一次性 UAC 提权发起（批次 5 UI 调用；本批不做真实 UAC 测试）=====
// ShellExecuteW "runas" selfExe，参数 --update-apply "<stagingDir>"（T10：
// 提权侧收到后必须自行 ValidateStagingPath，本函数只负责发起）。
// 用户拒绝 UAC → false + "已取消"。
bool LaunchElevatedApply(const std::wstring& selfExe, const std::wstring& stagingDir,
                         std::wstring* err);

} // namespace updater
} // namespace lingxi
