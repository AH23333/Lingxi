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
CellImportResult ImportCellPackSync(const std::wstring& srcPath,
                                    const std::unordered_set<std::string>& validSylls,
                                    bool overwrite);
} // namespace lingxi
