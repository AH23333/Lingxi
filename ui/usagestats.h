#pragma once
//
// 打字统计（LingxiUI 宿主进程内）：今日/累计选词次数、上屏汉字数、
// 活跃天数、连续使用天数。数据文件 APPDATA\LingxiIme\usage.txt。
// - 计数点：PipeServer::ApplyLearn（UI 点选与 IME 数字键选词的公共落点）
// - 节流落盘：每 20 次选词 + 退出/会话结束（StatsFlush）
// - 沙箱隔离：LINGXI_SETTINGS_NS 非空（deep_test/e2e 拉起的测试宿主）
//   自动停用，回归跑分不污染真实统计
//
#include <cstdint>
#include <string>

namespace lingxi {

struct UsageData
{
    uint32_t dayKey = 0;      // 今日 YYYYMMDD（0=尚无记录）
    uint64_t dayPicks = 0;    // 今日选词次数
    uint64_t dayChars = 0;    // 今日上屏汉字数
    uint64_t totalPicks = 0;  // 累计选词次数
    uint64_t totalChars = 0;  // 累计上屏汉字数
    uint32_t activeDays = 0;  // 有选词的天数
    uint32_t streakDays = 0;  // 连续使用天数（断一天即重开）
    uint32_t lastDayKey = 0;  // 最近一个活跃日（YYYYMMDD）
    uint64_t peakDayChars = 0; // 历史最高单日上屏汉字数（R8 统计增强）
    uint32_t peakDayKey = 0;  // 最高单日对应日期（YYYYMMDD；0=尚无）
    uint32_t goalReachedDay = 0; // 每日目标达成标记日（防当日重复提醒）
};

// 选词反馈计数（word 空则跳过；内部处理跨天翻转/连续天数/节流落盘）
void StatsAddPick(const wchar_t* word);
// 标记"每日目标达成"（R8）：写 goalReachedDay 防当日重复提醒并落盘
void StatsMarkGoalReached(uint32_t dayKey);
// 脏数据落盘（退出/会话结束时调用；无脏数据为空操作）
void StatsFlush();
// 当前统计快照（首次调用懒加载 usage.txt）
UsageData StatsSnapshot();
// 清零全部统计并落盘（不可恢复）
void StatsReset();
// 统计窗正文（多行文本，\r\n 结尾）
std::wstring BuildStatsText();
// 单行摘要（诊断报告等紧凑场景）
std::wstring BuildStatsBrief();

} // namespace lingxi
