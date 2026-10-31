#pragma once
//
// 每日自动备份（数据安全）：全部用户数据文本文件的按日快照 + 轮换。
// 目录：%APPDATA%\LingxiIme\backups\daily-YYYYMMDD\（当日幂等，覆盖同名）。
// 内容：userdict / blocked / phrases / hotphrases / usage 五文件（存在才拷，
// 未用过的数据文件跳过）。语义=当日首次打字选词时快照（捕获的是前一天
// 的最终数据，标准日备节奏）。
// 轮换：保留最近 14 个 daily-* 目录（目录名字典序=时间序），超限删最旧。
// 触发：AutoBackupTick()（打字学习路径，内部当日短路，µs 级开销）；
//      AutoBackupNow()（托盘"立即备份"，绕过当日短路）。
// 沙箱：LINGXI_SETTINGS_NS 非空（测试宿主）自动停用，防回归污染真实备份
// （与 usagestats 同约定）。
//
#include <string>
#include <cstdint>

namespace lingxi {

void AutoBackupTick();       // 高频路径节流入口：当日已备份则空操作
bool AutoBackupNow();        // 立即全量备份（false=沙箱停用/无文件可备/写失败）
std::wstring BackupsDirW();  // backups 根目录（托盘"打开备份文件夹"用）
uint32_t DailyBackupCount(); // daily-* 目录份数（--diagnostics 摘要用）

} // namespace lingxi
