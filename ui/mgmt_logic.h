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
#include <atomic>
#include <cwctype>
#include <string>
#include <utility>
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

// 解析用户词典列表行 "word\tkey\tcount"（RefreshUdList 的显示格式，N5 起
// tab 分隔——旧 4 空格分隔在 word/key 含 ≥4 连续空格时切分错位解析错目标，
// 定宽方案在字段超宽时同样错位；拼音键与词条均不含 tab，无歧义）。删除/
// 提升/降低一律按行内容定位词条——引擎自学习随时重写词典，列表索引与文件
// 会漂移，按索引定位会删错/落空。
inline bool ParseUdDisplayLine(const std::wstring& line,
                               std::wstring& word, std::wstring& key)
{
    word.clear(); key.clear();
    size_t t1 = line.find(L'\t');
    if (t1 == std::wstring::npos) return false;
    size_t t2 = line.find(L'\t', t1 + 1);
    if (t2 == std::wstring::npos) return false;
    word = line.substr(0, t1);
    key = line.substr(t1 + 1, t2 - t1 - 1);
    return !word.empty() && !key.empty();
}

// 批量操作目标：按行内容定位出的 (word,key)
struct UdTarget { std::wstring word, key; };

// 选择集 → 操作目标：selFlags[i] 非零=第 i 行选中（LB_GETSEL 语义）；
// 顺序保持列表序；选中但解析失败的行跳过并计入 skipped（N5：不静默——
// 调用方须在结果文案里报告"跳过 N 条"，刚被并发重写/异常行不再无声消失）。
inline std::vector<UdTarget> CollectSelected(const std::vector<std::wstring>& lines,
                                             const std::vector<int>& selFlags,
                                             size_t& skipped)
{
    std::vector<UdTarget> out;
    skipped = 0;
    size_t n = lines.size() < selFlags.size() ? lines.size() : selFlags.size();
    for (size_t i = 0; i < n; i++)
    {
        if (!selFlags[i]) continue;
        UdTarget t;
        if (ParseUdDisplayLine(lines[i], t.word, t.key)) out.push_back(std::move(t));
        else skipped++;
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

// N8b 启用合计限额裁决（镜像引擎 RebuildCellPacks：调用方先剔停用表/去重 id，
// 本函数按 id 升序累计，从超限包起整包不生效——与引擎 break 口径一致）。
// 返回指定 id 的生效条数（0=不生效/不存在）。引擎_test MG9 钉口径。
inline size_t CellQuotaEffective(std::vector<std::pair<std::wstring, size_t>> enabled,
                                 const std::wstring& id, size_t cap)
{
    std::sort(enabled.begin(), enabled.end(),
              [](const std::pair<std::wstring, size_t>& a,
                 const std::pair<std::wstring, size_t>& b) { return a.first < b.first; });
    size_t total = 0;
    for (const auto& p : enabled)
    {
        if (total + p.second > cap) break; // 引擎口径：超限整包跳，后续皆不生效
        total += p.second;
        if (p.first == id) return p.second;
    }
    return 0;
}

// N9 单槽所有权传递（至多一个在途——调用方须以防重入保证）：投递方 Register
// 登记后若投递失败须 Claim 回收（仅当领回自己的指针才 delete——窗口侧可能已
// 先一步 Claim 走）；窗口侧处理消息/销毁各 Claim 一次，后到者得 nullptr。
// 任意时序下指针恰好被释放一次，不悬在窗口队列里随窗销毁（关窗竞态泄漏根治）。
// engine_test MG10 钉语义。
template <typename T>
class PendingSlot
{
public:
    // 登记新指针；返回被顶掉的旧指针（调用方负责释放；正常路径恒 nullptr）
    T* Register(T* p) { return slot_.exchange(p, std::memory_order_acq_rel); }
    // 领取槽内指针并置空（后到者/空槽得 nullptr）
    T* Claim() { return slot_.exchange(nullptr, std::memory_order_acq_rel); }
private:
    std::atomic<T*> slot_{ nullptr };
};

} // namespace mgmt
} // namespace lingxi
