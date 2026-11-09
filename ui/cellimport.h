// cellimport.h - A 细胞词库导入适配层（design-cell-dict-2026-11-03 §4/A2/A6）
//
// 线程模型（裁决⑤红线）：本层只碰磁盘与内存副本，绝不触碰引擎 _entries/索引；
// 可在后台线程调用。导入成功后由 UI 主线程经 hooks 调 IPredictor::ReloadCellPacks()
// （该调用只置原子标志，真正重建在预测线程 Predict 入口）。
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace lingxi {

// 细胞包目录（与引擎 CellDir 同约定：%APPDATA%\LingxiIme\cell；UI 侧宽字符版，
// 导入时负责创建目录——引擎加载侧绝不创建）。
std::wstring CellPackDirW();

struct CellImportResult
{
    bool ok = false;
    std::wstring message; // 人读结果："成功 N 跳过 M（前5行号…）" 或拒绝/未确认原因
    std::string id;       // 包 id（成功时有效）
    size_t imported = 0;
    size_t skipped = 0;
};

// 同步导入一个词库包文件（调用方决定线程）：
//  1) 读源文件（>64MB 直接拒绝——防御外来巨文件）；
//  2) 解析：.lxcell 原生；缺魔数时回退深蓝词库转换(imewlconverter)纯文本产物
//     宽容解析（CJK 列=词、ASCII 字母列=音节、数字列=词频忽略；列式未实测，
//     启发式分类，非法行按跳过计数）；
//  3) 落盘名 <id>.lxcell：已存在且 !overwrite → 拒绝；overwrite → rename-aside
//     旧件（PITFALLS #89），后续任何失败恢复 aside；
//  4) 规范化重写为 .lxcell 标准头+词条（tmp+rename 原子落盘）；
//  5) 独立复核（A6，9e3f5dc 教训）：重读落盘件再解析，id 与词条数逐项比对，
//     不一致 → 删新件、恢复 aside、报"导入未确认"——禁止只看写盘返回值报成功。
// fallbackStem：深蓝纯文本回退解析时的显示名/id 来源 stem（UTF-8 转换前的
// 宽字符原样传入；空=沿用 srcPath 文件名）。在线导入的 staging 文件名是
// "lingxi-cell-dl-<pid>-<tick>"无意义占位——调用方应传 URL 基名，
// 否则在线导入的纯文本词库会以临时名落盘（2026-11-09 审查修正）。
CellImportResult ImportCellPackSync(const std::wstring& srcPath,
                                    const std::unordered_set<std::string>& validSylls,
                                    bool overwrite,
                                    const std::wstring& fallbackStem = L"");

// N7：清理 cell 目录下 rename-aside 残留的 *.del（删除时被占用改名让位的死件）。
// 返回成功删除数；failed 非空时回填失败数（仍被占用等——调用方须记日志不静默）。
size_t CleanupCellDelResidue(size_t* failed);

// N8a：文件名 stem（UTF-8）→ 包 id（小写字母/数字/-）。归一化丢失信息
//（含 CJK 字节、大小写、首尾剥离）时追加 stem 的 FNV-1a 哈希后缀去重——
// 纯 CJK 文件名不再全部归一成 "pack" 互相顶包。engine_test CELLc-7 钉口径。
std::string MakeIdFromStem(const std::string& stem);

// ---- 在线下载导入（细胞词库在线源，2026-11-06）----
// 管线：URL 形闸（cellfetch_logic.h：https/.lxcell/无 userinfo）→ 下载到
// %TEMP% staging（≤8MB）→ 完整性锚（显式 64-hex，或留空走同 origin 侧车
// <url>.sha256；两者皆无=拒绝安装，fail-closed）→ 复核通过走与本地导入
// 完全一致的 ImportCellPackSync（解析/原子落盘/独立复核，不新增攻击面）。
// 域名白名单由下载层二次把关（updater::DownloadToFile → IsUrlAllowed：
// GitHub 精确匹配；回环仅演练钩子 LINGXI_UPDATE_TEST_BASE 放行）。
// 线程模型同 ImportCellPackSync（可在后台线程调用；hooks 通知在主线程）。
// downloader 默认空 = updater::DownloadToFile 实网；测试注入假实现（零网络）。
// 任何失败 staging 两件（.lxcell/.sha256）全清。
typedef bool (*CellDownloadFn)(const std::wstring& url, const std::wstring& destPath,
                               size_t maxBytes, std::wstring* err);
CellImportResult ImportCellPackFromUrl(const std::wstring& url,
                                       const std::wstring& sha256HexOrEmpty,
                                       const std::unordered_set<std::string>& validSylls,
                                       bool overwrite,
                                       CellDownloadFn downloader = nullptr);
} // namespace lingxi
