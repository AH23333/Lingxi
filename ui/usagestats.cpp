//
// 打字统计实现。文件格式（UTF-8，容错解析）：key=value 行
//   v / day / dayPicks / dayChars / totalPicks / totalChars /
//   activeDays / streak / lastDay
// 跨天翻转与连续天数用纯 YYYYMMDD 日期回退（PrevDayKey），无时区问题。
//
#include "usagestats.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../common/settings.h"

namespace lingxi {

namespace {

std::mutex g_mtx;
UsageData g_data;
bool g_loaded = false;   // 懒加载只做一次
bool g_disabled = false; // 测试沙箱宿主：全部读写停用
uint32_t g_pending = 0;  // 自上次落盘以来的选词次数（节流计数）

std::wstring StatsFilePath()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring dir = std::wstring(buf) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\usage.txt";
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe) + L"usage.txt";
}

// 原子写：先落 .tmp 再改名替换（与引擎字典文件同模式）——
// CREATE_ALWAYS 直写有两个撕裂窗口：进程中途被杀留下半截 usage.txt（下次
// 启动解析出部分值）；写入期间备份 CopyFileW / 诊断导出恰好读到空/半截文件。
bool WriteTextFile(const std::wstring& path, const std::string& text)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, NULL) &&
              written == text.size();
    if (ok) ok = FlushFileBuffers(h) != FALSE; // 审查缺陷④：补齐刷盘（批8 口径，断电下未刷盘的 .tmp 不算落盘）
    CloseHandle(h);
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str()); // 审查缺陷④：rename 失败不留 .tmp 残留（与其余写者同款）
        return false;
    }
    return true;
}

// YYYYMMDD → 前一天（纯日期回退：闰年二月/跨月/跨年）
uint32_t PrevDayKey(uint32_t key)
{
    uint32_t y = key / 10000, m = (key / 100) % 100, d = key % 100;
    static const uint32_t mdays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (d > 1)
        d--;
    else if (m > 1)
    {
        m--;
        uint32_t feb = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0;
        d = mdays[m] + (m == 2 ? feb : 0);
    }
    else
    {
        y--;
        m = 12;
        d = 31;
    }
    return y * 10000 + m * 100 + d;
}

uint32_t TodayKey()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return (uint32_t)st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

// 汉字计数：CJK 基本区+扩展A+兼容表意（口径=候选窗选词上屏的汉字数）
uint32_t CountCjk(const wchar_t* w)
{
    uint32_t n = 0;
    for (const wchar_t* p = w; p && *p; p++)
        if ((*p >= 0x3400 && *p <= 0x9FFF) || (*p >= 0xF900 && *p <= 0xFAFF))
            n++;
    return n;
}

// 首次访问：沙箱检测 + usage.txt 懒加载（缺失=全零起步）
void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    // 读 main.cpp 的快照（而非直读环境变量）：生产宿主启动即清 LINGXI_SETTINGS_NS，
    // e2e 派生的宿主若惰性读环境会误判生产，把打字统计写进 regress 临时 APPDATA
    if (WasTestSpawned())
    {
        g_disabled = true;
        return;
    }
    HANDLE h = CreateFileW(StatsFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string raw;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, NULL) && n) raw.append(buf, n);
    CloseHandle(h);
    size_t pos = (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
    while (pos < raw.size())
    {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos)
        {
            const char* k = line.c_str();
            unsigned long long v = _strtoui64(k + eq + 1, NULL, 10);
            if (strncmp(k, "day=", 4) == 0) g_data.dayKey = (uint32_t)v;
            else if (strncmp(k, "dayPicks=", 9) == 0) g_data.dayPicks = v;
            else if (strncmp(k, "dayChars=", 9) == 0) g_data.dayChars = v;
            else if (strncmp(k, "totalPicks=", 11) == 0) g_data.totalPicks = v;
            else if (strncmp(k, "totalChars=", 11) == 0) g_data.totalChars = v;
            else if (strncmp(k, "activeDays=", 11) == 0) g_data.activeDays = (uint32_t)v;
            else if (strncmp(k, "streak=", 7) == 0) g_data.streakDays = (uint32_t)v;
            else if (strncmp(k, "lastDay=", 8) == 0) g_data.lastDayKey = (uint32_t)v;
            else if (strncmp(k, "peakChars=", 10) == 0) g_data.peakDayChars = v;
            else if (strncmp(k, "peakDay=", 8) == 0) g_data.peakDayKey = (uint32_t)v;
            else if (strncmp(k, "goalDay=", 8) == 0) g_data.goalReachedDay = (uint32_t)v;
        }
        pos = eol + 1;
    }
}

void SaveLocked()
{
    if (g_disabled || !g_loaded) return;
    // L9：满值格式串（七个 %llu 全 20 位）约 283 字节，256 截尾丢末段
    // goalDay（_TRUNCATE 不崩但数据残缺，次日回读归 0）；512 余量充足
    char b[512];
    _snprintf_s(b, _TRUNCATE,
        "v=1\r\nday=%u\r\ndayPicks=%llu\r\ndayChars=%llu\r\n"
        "totalPicks=%llu\r\ntotalChars=%llu\r\nactiveDays=%u\r\n"
        "streak=%u\r\nlastDay=%u\r\npeakChars=%llu\r\npeakDay=%u\r\ngoalDay=%u\r\n",
        (unsigned)g_data.dayKey, (unsigned long long)g_data.dayPicks,
        (unsigned long long)g_data.dayChars,
        (unsigned long long)g_data.totalPicks, (unsigned long long)g_data.totalChars,
        (unsigned)g_data.activeDays, (unsigned)g_data.streakDays,
        (unsigned)g_data.lastDayKey, (unsigned long long)g_data.peakDayChars,
        (unsigned)g_data.peakDayKey, (unsigned)g_data.goalReachedDay);
    WriteTextFile(StatsFilePath(), b);
    g_pending = 0;
}

void FormatDayKey(uint32_t key, wchar_t* out, size_t cch)
{
    if (key == 0)
    {
        wcscpy_s(out, cch, L"—");
        return;
    }
    _snwprintf_s(out, cch, _TRUNCATE, L"%u-%02u-%02u",
                 key / 10000, (key / 100) % 100, key % 100);
}

} // namespace

void StatsAddPick(const wchar_t* word)
{
    if (!word || !*word) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    LoadLocked();
    if (g_disabled) return;
    uint32_t today = TodayKey();
    if (today != g_data.dayKey) // 跨天：今日清零 + 连续天数判定
    {
        g_data.streakDays = (g_data.lastDayKey != 0 && PrevDayKey(today) == g_data.lastDayKey)
                            ? g_data.streakDays + 1 : 1;
        g_data.dayKey = today;
        g_data.dayPicks = 0;
        g_data.dayChars = 0;
        g_data.activeDays++;
    }
    g_data.lastDayKey = today;
    uint32_t cjk = CountCjk(word);
    g_data.dayPicks++;
    g_data.totalPicks++;
    g_data.dayChars += cjk;
    g_data.totalChars += cjk;
    if (g_data.dayChars > g_data.peakDayChars) // 历史最高单日：实时刷新（R8）
    {
        g_data.peakDayChars = g_data.dayChars;
        g_data.peakDayKey = today;
    }
    if (++g_pending >= 20) SaveLocked(); // 节流落盘：崩溃最多丢 20 次
}

void StatsMarkGoalReached(uint32_t dayKey)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    LoadLocked();
    if (g_disabled || g_data.goalReachedDay == dayKey) return;
    g_data.goalReachedDay = dayKey;
    SaveLocked();
}

void StatsFlush()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    LoadLocked();
    if (g_pending > 0) SaveLocked();
}

UsageData StatsSnapshot()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    LoadLocked();
    return g_data;
}

void StatsReset()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    LoadLocked();
    if (g_disabled) return;
    g_data = UsageData{};
    SaveLocked();
}

std::wstring BuildStatsText()
{
    UsageData d = StatsSnapshot();
    if (d.totalPicks == 0)
        return L"灵犀输入法 · 打字统计\r\n\r\n"
               L"尚无选词记录。\r\n\r\n"
               L"（通过候选窗选词上屏后开始统计；每 20 次选词落盘一次，"
               L"退出时保存。）\r\n";
    wchar_t day[16];
    FormatDayKey(d.dayKey, day, 16);
    wchar_t peakDay[16];
    FormatDayKey(d.peakDayKey, peakDay, 16);
    wchar_t buf[512];
    _snwprintf_s(buf, _TRUNCATE,
        L"灵犀输入法 · 打字统计\r\n\r\n"
        L"【今日 %ls】\r\n· 选词：%llu 次\r\n· 上屏：%llu 个汉字\r\n\r\n"
        L"【累计】\r\n· 选词：%llu 次\r\n· 上屏：%llu 个汉字\r\n"
        L"· 活跃天数：%u 天（连续 %u 天）\r\n"
        L"· 历史最高单日：%llu 个汉字（%ls）\r\n",
        day,
        (unsigned long long)d.dayPicks, (unsigned long long)d.dayChars,
        (unsigned long long)d.totalPicks, (unsigned long long)d.totalChars,
        (unsigned)d.activeDays, (unsigned)d.streakDays,
        (unsigned long long)d.peakDayChars, peakDay);
    std::wstring text(buf);
    // 每日目标进度行（R8）：目标=0 关闭不显示；达成后带 ✓ 标记
    uint32_t goal = LoadSettings().dailyCharGoal;
    if (goal > 0)
    {
        unsigned pct = d.dayChars >= goal ? 100 :
                       (unsigned)(d.dayChars * 100 / goal);
        wchar_t g[96];
        _snwprintf_s(g, _TRUNCATE, L"\r\n【今日目标】%llu / %u 字（%u%%）%ls\r\n",
                     (unsigned long long)d.dayChars, (unsigned)goal, pct,
                     d.dayChars >= goal ? L"✓ 已达成" : L"");
        text += g;
    }
    return text +
        L"\r\n（口径：候选窗选词上屏；每 20 次选词落盘，退出时保存。"
        L"清零不可恢复。）\r\n";
}

std::wstring BuildStatsBrief()
{
    UsageData d = StatsSnapshot();
    wchar_t buf[160];
    if (d.totalPicks == 0)
        wcscpy_s(buf, L"尚无选词记录");
    else
        _snwprintf_s(buf, _TRUNCATE,
            L"今日选词 %llu 次/%llu 字，累计 %llu 次/%llu 字，活跃 %u 天",
            (unsigned long long)d.dayPicks, (unsigned long long)d.dayChars,
            (unsigned long long)d.totalPicks, (unsigned long long)d.totalChars,
            (unsigned)d.activeDays);
    return buf;
}

} // namespace lingxi
