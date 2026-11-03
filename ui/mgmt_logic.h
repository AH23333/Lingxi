#pragma once
//
// ui/mgmt_logic.h — 管理窗纯逻辑层（批14：搜索过滤 / 选择集 / 批量启停）。
// 用户词典窗（userdictwnd.cpp）与细胞词库管理窗（cellwnd.cpp）共用本头：
// "哪些行被过滤命中、行内容如何定位词条、选择集→操作目标、批量启停后
// 停用表长什么样"全部判定在此，Win32 消息循环保留控件读写与引擎钩子调用。
// engine_test 直链本头单测（照 cand_text.h 先例，header-only 无链接依赖）；
// 视觉与交互（多选高亮、搜索框实时刷新观感）blocked 待真机。
//
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace lingxi {
namespace mgmt {

inline std::wstring ToLowerW(const std::wstring& s)
{
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t ch : s) r.push_back((wchar_t)towlower(ch));
    return r;
}

// 用户词典过滤：filter 为词条或拼音键的子串即命中（不区分大小写）；
// 空 filter = 全量放行（清空搜索框恢复全量）。
inline bool UdFilterMatch(const std::wstring& word, const std::wstring& key,
                          const std::wstring& filter)
{
    if (filter.empty()) return true;
    std::wstring fl = ToLowerW(filter);
    return ToLowerW(word).find(fl) != std::wstring::npos ||
           ToLowerW(key).find(fl) != std::wstring::npos;
}

// 细胞词库包过滤：id 或包名子串命中（不区分大小写）；空 filter = 全量。
inline bool CellFilterMatch(const std::wstring& id, const std::wstring& name,
                            const std::wstring& filter)
{
    if (filter.empty()) return true;
    std::wstring fl = ToLowerW(filter);
    return ToLowerW(id).find(fl) != std::wstring::npos ||
           ToLowerW(name).find(fl) != std::wstring::npos;
}

// 解析用户词典列表行 "word␣␣␣key␣␣␣count"（RefreshUdList 的显示格式，
// 4 空格分隔）。删除/提升/降低一律按行内容定位词条——引擎自学习随时
// 重写词典，列表索引与文件会漂移，按索引定位会删错/落空。
inline bool ParseUdDisplayLine(const std::wstring& line,
                               std::wstring& word, std::wstring& key)
{
    word.clear(); key.clear();
    size_t p1 = line.find(L"    ");
    if (p1 == std::wstring::npos) return false;
    size_t p2 = line.find(L"    ", p1 + 4);
    if (p2 == std::wstring::npos) return false;
    word = line.substr(0, p1);
    key = line.substr(p1 + 4, p2 - p1 - 4);
    return !word.empty() && !key.empty();
}

// 批量操作目标：按行内容定位出的 (word,key)
struct UdTarget { std::wstring word, key; };

// 选择集 → 操作目标：selFlags[i] 非零=第 i 行选中（LB_GETSEL 语义）；
// 顺序保持列表序；选中但解析失败的行跳过（刚被并发重写消失的词条）。
inline std::vector<UdTarget> CollectSelected(const std::vector<std::wstring>& lines,
                                             const std::vector<int>& selFlags)
{
    std::vector<UdTarget> out;
    size_t n = lines.size() < selFlags.size() ? lines.size() : selFlags.size();
    for (size_t i = 0; i < n; i++)
    {
        if (!selFlags[i]) continue;
        UdTarget t;
        if (ParseUdDisplayLine(lines[i], t.word, t.key)) out.push_back(std::move(t));
    }
    return out;
}

// 停用表 CSV（逗号分隔包 id）解析：去首尾空格/制表符、跳空段
inline std::vector<std::wstring> SplitIds(const std::wstring& csv)
{
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= csv.size())
    {
        size_t c = csv.find(L',', pos);
        std::wstring seg = csv.substr(pos, c == std::wstring::npos ? c : c - pos);
        while (!seg.empty() && (seg.front() == L' ' || seg.front() == L'\t'))
            seg.erase(seg.begin());
        while (!seg.empty() && (seg.back() == L' ' || seg.back() == L'\t'))
            seg.pop_back();
        if (!seg.empty()) out.push_back(seg);
        if (c == std::wstring::npos) break;
        pos = c + 1;
    }
    return out;
}

inline std::wstring JoinIds(const std::vector<std::wstring>& ids)
{
    std::wstring joined;
    for (size_t i = 0; i < ids.size(); i++)
    {
        if (i) joined += L',';
        joined += ids[i];
    }
    return joined;
}

// 批量启停（Cell.disabled 唯一写入口）：返回新停用表 CSV。
// dis=true 把未停用项加入（去重，原顺序保持、新增项追加尾部）；
// dis=false 把列出的 id 全部移出（启用）。整批一次落盘（单次 SaveSettings），
// 引擎在下一次 Predict 检出 SettingsSeq 变更后自动重建。
inline std::wstring ApplyBulkDisabled(const std::wstring& csv,
                                      const std::vector<std::wstring>& ids, bool dis)
{
    std::vector<std::wstring> cur = SplitIds(csv);
    if (dis)
    {
        for (const auto& id : ids)
            if (std::find(cur.begin(), cur.end(), id) == cur.end()) cur.push_back(id);
    }
    else
    {
        for (const auto& id : ids)
            cur.erase(std::remove(cur.begin(), cur.end(), id), cur.end());
    }
    return JoinIds(cur);
}

} // namespace mgmt
} // namespace lingxi
