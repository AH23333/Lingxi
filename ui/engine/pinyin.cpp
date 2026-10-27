//
// M5 拼音转换引擎：原始拼音字母串 → 汉字词链候选。
//
// 数据：<exe目录>\data\zh_pinyin.txt（词<TAB>音节1 音节2 ...，全小写，按词频降序）。
// 加载方式与 ngram.cpp 一致：GetModuleFileName(NULL) 拼 exe 目录，构造期一次性加载。
//
// Predict 流程（≤10ms；所有索引均在构造期预构建，Predict 阶段零全表扫描）：
//   1. 过滤 context → 纯小写字母串（去空格等杂质，大写折小写）；
//   2. 音节切分：DP 求前 3 条最优路径（费用 = 2*音节数，少者优，即优先长音节），
//      允许末段为某音节的真前缀（残段，如 "q"），残段路径记 +1 罚分；
//   3. 词链生成：对每条切分路径做贪心拼接——每步取后续 1..6 个音节拼成紧凑 key
//      （如 "jintian"）查紧凑序列索引，最长匹配优先、桶内按词频序取词；
//      DFS 带有界回溯，避免贪心跳进死路；末尾残段用"首音节分桶 + 线性过滤"
//      匹配"首音节以残段开头"的词（残段恰为完整读音的桶优先）；
//   4. 候选组装：最优整链 + 首词换 2/3 频词的替代链 + 仅首词候选，
//      按序去重取前 5；一条链都拼不出来则该路径不产候选，全空则返回空。
//
// 质量定位（诚实占位）：贪心 + 首词替代，非全局 k-best；M5.5 可升级全局 DP。
//
#include "predictor.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lingxi {

namespace {

constexpr size_t kMaxCands = 9;        // 候选上限（数字键 1-9 对应）
constexpr size_t kMaxChainSpan = 6;    // 单词最多拼接的音节数
constexpr size_t kMaxPaths = 3;        // 切分保留的最优路径数
constexpr size_t kMaxLetters = 50;     // 参与切分的字母数上限（超出保留尾部）
constexpr int kDfsBudget = 512;        // 词链 DFS 节点预算（有界回溯，远小于 10ms）
constexpr size_t kOptsPerSpan = 4;     // 每个跨度最多取的候选词数（桶内按词频序）
constexpr size_t kMaxOpts = 16;        // 每个位置的可选词上限
constexpr size_t kMaxResidualOpts = 12; // 残段匹配的可选词上限

inline bool IsLowerLetter(wchar_t ch) { return ch >= L'a' && ch <= L'z'; }

inline bool IsCjkChar(wchar_t ch)
{
    return (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF);
}

struct PyEntry
{
    std::wstring word; // 汉字词
    uint32_t rank = 0; // 文件序 = 词频名次，越小越常用
};

// 词链贪心的一个可选词
struct WordOpt
{
    uint32_t span;  // 该词消耗的音节数（残段收尾恒为 1）
    uint32_t entry; // _entries 下标
};

// 一条音节切分路径
struct SegPath
{
    uint32_t cost = 0;        // 2*完整音节数 +（末段残段 ? 1 : 0）
    bool partialTail = false; // 末音节是否残段
    std::vector<std::wstring> sylls;
};

class PinyinEngine : public IPredictor
{
public:
    PinyinEngine() { Load(); }

    Prediction Predict(const std::wstring& ctx) override
    {
        Prediction pred;
        std::wstring letters;
        letters.reserve(ctx.size());
        for (wchar_t ch : ctx)
        {
            if (ch >= L'A' && ch <= L'Z') ch = wchar_t(ch - L'A' + L'a');
            if (IsLowerLetter(ch)) letters.push_back(ch);
        }
        if (letters.empty() || _entries.empty()) return pred;
        if (letters.size() > kMaxLetters)
            letters = letters.substr(letters.size() - kMaxLetters);

        std::vector<std::wstring> pool;
        for (const SegPath& path : Segment(letters))
            CollectPathCandidates(path, pool);

        // 按序去重，取前 5
        std::unordered_set<std::wstring> seen;
        for (const std::wstring& c : pool)
        {
            if (c.empty()) continue;
            if (!seen.insert(c).second) continue;
            pred.cands.push_back(c);
            if (pred.cands.size() >= kMaxCands) break;
        }
        return pred; // replaceLen 恒 0：DLL 用组合串长度整体替换
    }

private:
    // ---------- 数据加载与索引 ----------

    static std::wstring DataPath(const wchar_t* file)
    {
        wchar_t exePath[MAX_PATH] = L"";
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) return L"";
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (!slash) return L"";
        *slash = L'\0';
        return std::wstring(exePath) + L"\\data\\" + file;
    }

    void Load()
    {
        std::ifstream f(DataPath(L"zh_pinyin.txt"), std::ios::binary);
        if (!f) return;
        // 整文件一次读入（istreambuf_iterator 逐字符在 Debug CRT 下过慢）
        f.seekg(0, std::ios::end);
        std::streamoff sz = f.tellg();
        if (sz <= 0) return;
        f.seekg(0, std::ios::beg);
        std::string raw((size_t)sz, '\0');
        f.read(&raw[0], sz);
        raw.resize((size_t)(f.gcount() > 0 ? f.gcount() : 0));
        int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), NULL, 0);
        if (wlen <= 0) return;
        std::wstring text(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &text[0], wlen);

        // 预留容量，避免 Debug CRT 下的反复扩容/重哈希
        _entries.reserve(text.size() / 13 + 64);
        _byCompact.reserve(text.size() / 13 + 64);
        _byFirstSyll.reserve(512);
        _syllSet.reserve(512);

        uint32_t rank = 0;
        size_t pos = 0;
        const size_t n = text.size();
        while (pos < n)
        {
            size_t eol = text.find(L'\n', pos);
            size_t end = (eol == std::wstring::npos) ? n : eol;
            if (end > pos && text[end - 1] == L'\r') end--; // 去 \r
            size_t tab = std::wstring::npos;
            for (size_t k = pos; k < end; k++)
            {
                if (text[k] == L'\t') { tab = k; break; }
            }
            if (tab != std::wstring::npos &&
                AddEntry(text.c_str() + pos, tab - pos, text.c_str() + tab + 1, end - tab - 1, rank))
            {
                rank++;
            }
            pos = (eol == std::wstring::npos) ? n : eol + 1;
        }

        // 音节表按首字母分组，组内按长度降序（切分时优先尝试长音节）
        for (const std::wstring& sy : _syllSet) _syllByFirst[sy[0]].push_back(sy);
        for (auto& kv : _syllByFirst)
        {
            std::sort(kv.second.begin(), kv.second.end(),
                      [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
        }
    }

    bool AddEntry(const wchar_t* word, size_t wlen, const wchar_t* seq, size_t slen, uint32_t rank)
    {
        if (wlen == 0 || wlen > 12) return false;
        for (size_t k = 0; k < wlen; k++)
            if (!IsCjkChar(word[k])) return false; // 纯汉字词

        // 单趟扫音节：拼紧凑串 + 校验纯小写字母 + 收集音节表；
        // 用可复用 scratch 查 _syllSet（命中时不产生新分配）
        std::wstring compact;
        compact.reserve(slen);
        std::wstring scratch;
        scratch.reserve(8);
        std::wstring firstSyll;
        uint32_t count = 0;
        size_t k = 0;
        while (k < slen)
        {
            while (k < slen && seq[k] == L' ') k++; // 跳过空格（含连续空格）
            size_t s = k;
            while (k < slen && seq[k] != L' ')
            {
                if (seq[k] < L'a' || seq[k] > L'z') return false; // 数据自检
                k++;
            }
            if (k > s)
            {
                if (count == 0) firstSyll.assign(seq + s, k - s);
                compact.append(seq + s, k - s);
                scratch.assign(seq + s, k - s);
                _syllSet.insert(scratch);
                count++;
                if (count > 16) return false;
            }
        }
        if (count == 0) return false;

        uint32_t idx = (uint32_t)_entries.size();
        _entries.push_back({ std::wstring(word, wlen), rank });
        _byCompact[std::move(compact)].push_back(idx);
        _byFirstSyll[std::move(firstSyll)].push_back(idx); // 追加序 = 词频序，桶内天然升序
        return true;
    }

    // ---------- 音节切分（DP，k 最优） ----------

    static bool PathLess(const SegPath& a, const SegPath& b)
    {
        if (a.cost != b.cost) return a.cost < b.cost;
        // 确定性 tie-break：逐音节字典序，再比长度
        size_t k = (std::min)(a.sylls.size(), b.sylls.size());
        for (size_t i = 0; i < k; i++)
        {
            int c = a.sylls[i].compare(b.sylls[i]);
            if (c != 0) return c < 0;
        }
        return a.sylls.size() < b.sylls.size();
    }

    // 插入并保持有序，截断到 kMaxPaths
    static void InsertPath(std::vector<SegPath>& v, SegPath&& p)
    {
        size_t pos = 0;
        while (pos < v.size() && PathLess(v[pos], p)) pos++;
        if (pos >= kMaxPaths) return;
        v.insert(v.begin() + pos, std::move(p));
        if (v.size() > kMaxPaths) v.resize(kMaxPaths);
    }

    std::vector<SegPath> Segment(const std::wstring& s) const
    {
        const size_t n = s.size();
        // dp[i]：覆盖 s[0..i) 的最优 k 条"完整音节"路径
        std::vector<std::vector<SegPath>> dp(n + 1);
        dp[0].push_back(SegPath{});
        for (size_t i = 0; i < n; i++)
        {
            if (dp[i].empty()) continue;
            auto it = _syllByFirst.find(s[i]);
            if (it == _syllByFirst.end()) continue;
            for (const std::wstring& sy : it->second) // 长度降序
            {
                size_t len = sy.size();
                if (i + len > n || s.compare(i, len, sy) != 0) continue;
                for (const SegPath& p : dp[i])
                {
                    SegPath np = p;
                    np.cost += 2;
                    np.sylls.push_back(sy);
                    InsertPath(dp[i + len], std::move(np));
                }
            }
        }

        std::vector<SegPath> all(std::move(dp[n]));
        // 末段残段：s[j..n) 是某更长音节的真前缀（恰为完整音节时 dp[n] 已覆盖且无罚分）
        for (size_t j = 0; j < n; j++)
        {
            if (dp[j].empty()) continue;
            std::wstring tail = s.substr(j, n - j);
            auto it = _syllByFirst.find(tail[0]);
            if (it == _syllByFirst.end()) continue;
            bool prefixOfLonger = false;
            for (const std::wstring& sy : it->second)
            {
                if (sy.size() > tail.size() && sy.compare(0, tail.size(), tail) == 0)
                {
                    prefixOfLonger = true;
                    break;
                }
            }
            if (!prefixOfLonger) continue;
            for (const SegPath& p : dp[j])
            {
                SegPath np = p;
                np.cost += 3; // 2 + 残段罚分 1
                np.sylls.push_back(tail);
                np.partialTail = true;
                InsertPath(all, std::move(np));
            }
        }
        return all;
    }

    // ---------- 词链生成（贪心 + 有界回溯） ----------

    // 残段匹配：首音节分桶 + 线性过滤（避免全表扫描）。
    // 桶键（完整音节）以残段开头即命中；桶内按词频序取头部；
    // 首音节恰等于残段的词优先（残段也可能已是完整读音，如 "ha" 之于 "hai"）。
    std::vector<WordOpt> ResidualOptions(const std::wstring& tail) const
    {
        std::vector<WordOpt> opts;
        auto exact = _byFirstSyll.find(tail);
        if (exact != _byFirstSyll.end())
        {
            size_t take = (std::min)(exact->second.size(), kOptsPerSpan);
            for (size_t k = 0; k < take; k++) opts.push_back({ 1, exact->second[k] });
        }
        size_t exactCount = opts.size();
        for (const auto& kv : _byFirstSyll)
        {
            const std::wstring& sy = kv.first;
            if (sy.size() <= tail.size() || sy.compare(0, tail.size(), tail) != 0) continue;
            size_t take = (std::min)(kv.second.size(), size_t(2));
            for (size_t k = 0; k < take; k++) opts.push_back({ 1, kv.second[k] });
        }
        // exact 段已按词频序；其余段合并后按词频序，exact 整体优先
        std::stable_sort(opts.begin() + exactCount, opts.end(),
                         [this](const WordOpt& a, const WordOpt& b)
                         { return _entries[a.entry].rank < _entries[b.entry].rank; });
        if (opts.size() > kMaxResidualOpts) opts.resize(kMaxResidualOpts);
        return opts;
    }

    // 位置 i 的可选词：跨度降序（最长匹配优先），同跨度桶内按词频序
    void BuildOptions(const std::vector<std::wstring>& sylls, size_t i, bool lastPartial,
                      std::vector<WordOpt>& opts) const
    {
        opts.clear();
        const size_t n = sylls.size();
        if (lastPartial && i + 1 == n)
        {
            opts = ResidualOptions(sylls[i]);
            return;
        }
        size_t maxSpan = (std::min)(kMaxChainSpan, n - i);
        for (size_t span = maxSpan; span >= 1; span--)
        {
            std::wstring key;
            for (size_t k = i; k < i + span; k++) key += sylls[k];
            auto it = _byCompact.find(key);
            if (it == _byCompact.end()) continue;
            size_t take = (std::min)(it->second.size(), kOptsPerSpan);
            for (size_t k = 0; k < take; k++)
                opts.push_back({ (uint32_t)span, it->second[k] });
            if (opts.size() >= kMaxOpts) break;
        }
        if (opts.size() > kMaxOpts) opts.resize(kMaxOpts);
    }

    // DFS 找第一条能走通的词链（选项按质量降序 → 首个成功即最优贪心链）
    bool DfsChain(const std::vector<std::wstring>& sylls, size_t i, bool lastPartial,
                  std::vector<WordOpt>& scratch, std::vector<WordOpt>& chain, int& budget) const
    {
        if (i == sylls.size()) return true;
        if (budget-- <= 0) return false;
        BuildOptions(sylls, i, lastPartial, scratch);
        std::vector<WordOpt> opts(scratch); // 拷贝：递归会复用 scratch，避免迭代器失效
        size_t mark = chain.size();
        for (const WordOpt& o : opts)
        {
            chain.push_back(o);
            if (DfsChain(sylls, i + o.span, lastPartial, scratch, chain, budget)) return true;
            chain.resize(mark);
        }
        return false;
    }

    std::wstring ChainText(const std::vector<WordOpt>& chain) const
    {
        std::wstring t;
        for (const WordOpt& o : chain) t += _entries[o.entry].word;
        return t;
    }

    // 词频加权 Viterbi：在音节序列上全局择优（代价 = Σ log(1+词频名次)，
    // 名次越小越常用；多词链代价项更多，自然偏好少词/高频组合）。
    struct DpNode
    {
        double cost = 0;
        int prev = -1;      // 后继位置（WalkChain 用）
        uint32_t entry = 0; // 本段词条
        uint32_t span = 0;  // 消耗音节数
        bool valid = false;
    };

    // 计算 [start, n) 的最优词链：dp[i] = 位置 i 选取的词 + 后继代价。
    // 复用 BuildOptions（各跨度桶内词频序），每位置每跨度取头部若干。
    void DpSuffix(const std::vector<std::wstring>& sylls, size_t start, bool lastPartial,
                  std::vector<DpNode>& dp) const
    {
        const size_t n = sylls.size();
        dp.assign(n + 1, DpNode{});
        dp[n].valid = true; // 终点
        for (size_t i = n; i-- > start;)
        {
            std::vector<WordOpt> opts;
            BuildOptions(sylls, i, lastPartial, opts);
            for (const WordOpt& o : opts)
            {
                if (i + o.span > n) continue;
                const DpNode& nxt = dp[i + o.span];
                if (!nxt.valid) continue;
                double c = std::log(1.0 + (double)_entries[o.entry].rank) + nxt.cost;
                if (!dp[i].valid || c < dp[i].cost)
                    dp[i] = { c, (int)(i + o.span), o.entry, o.span, true };
            }
        }
    }

    static bool WalkChain(const std::vector<DpNode>& dp, size_t start, std::vector<WordOpt>& chain)
    {
        if (start >= dp.size() || !dp[start].valid) return false;
        size_t i = start;
        while (i + 1 < dp.size())
        {
            const DpNode& nd = dp[i];
            if (!nd.valid) return false;
            chain.push_back({ nd.span, nd.entry });
            i = (size_t)nd.prev;
        }
        return !chain.empty();
    }

    // 候选组装：Viterbi 最优整链 + 末字变体（最后一音节的其他高频字）+
    // 首词替代链 + 仅首词；按序去重取前 9。
    void CollectPathCandidates(const SegPath& path, std::vector<std::wstring>& pool) const
    {
        const std::vector<std::wstring>& sylls = path.sylls;
        const size_t n = sylls.size();

        std::vector<DpNode> dp;
        DpSuffix(sylls, 0, path.partialTail, dp);

        std::vector<WordOpt> chain;
        if (!WalkChain(dp, 0, chain))
        {
            // 兑底：贪心 DFS（旧实现）
            std::vector<WordOpt> scratch;
            int budget = kDfsBudget;
            if (DfsChain(sylls, 0, path.partialTail, scratch, chain, budget))
            {
                pool.push_back(ChainText(chain));
                std::vector<WordOpt> firstOpts;
                BuildOptions(sylls, 0, path.partialTail, firstOpts);
                if (!firstOpts.empty()) pool.push_back(_entries[firstOpts[0].entry].word);
            }
            else
            {
                std::vector<WordOpt> firstOpts;
                BuildOptions(sylls, 0, path.partialTail, firstOpts);
                if (!firstOpts.empty()) pool.push_back(_entries[firstOpts[0].entry].word);
            }
            return;
        }

        std::wstring firstWord = _entries[chain[0].entry].word;
        pool.push_back(ChainText(chain)); // 1) 全局最优整链

        // 2) 末字变体：最后一音节的其他高频字（今天其 → 今天气/今天起/今天期…）。
        //    条件：末词只覆盖最后一个音节（单字收尾）。
        if (!chain.empty() && chain.back().span == 1)
        {
            std::vector<WordOpt> lastOpts;
            BuildOptions(sylls, n - 1, path.partialTail, lastOpts);
            std::wstring prefixText;
            for (size_t k = 0; k + 1 < chain.size(); k++) prefixText += _entries[chain[k].entry].word;
            int variants = 0;
            std::wstring curLast = _entries[chain.back().entry].word;
            for (const WordOpt& o : lastOpts)
            {
                const PyEntry& e = _entries[o.entry];
                if (e.word == curLast) continue;
                pool.push_back(prefixText + e.word);
                if (++variants >= 3) break;
            }
        }

        // 3) 首词替代链：换位置 0 的其他词，其余用 DP 最优补完
        std::vector<WordOpt> firstOpts;
        BuildOptions(sylls, 0, path.partialTail, firstOpts);
        int alts = 0;
        for (size_t k = 1; k < firstOpts.size() && alts < 2; k++)
        {
            const PyEntry& e = _entries[firstOpts[k].entry];
            if (e.word == firstWord) continue;
            if (firstOpts[k].span >= n)
            {
                pool.push_back(e.word); // 单词覆盖全部音节
            }
            else
            {
                std::vector<DpNode> dp2;
                DpSuffix(sylls, firstOpts[k].span, path.partialTail, dp2);
                std::vector<WordOpt> rest;
                if (!WalkChain(dp2, firstOpts[k].span, rest))
                    pool.push_back(e.word);
                else
                {
                    std::wstring alt = e.word;
                    for (const WordOpt& o : rest) alt += _entries[o.entry].word;
                    pool.push_back(std::move(alt));
                }
            }
            alts++;
        }

        // 4) 仅首词（部分输入/前缀场景有用；重复由去重消化）
        if (chain.size() > 1) pool.push_back(std::move(firstWord));
    }

    // ---------- 成员 ----------
    std::vector<PyEntry> _entries;
    std::unordered_map<std::wstring, std::vector<uint32_t>> _byCompact;    // 紧凑拼音 → 词条
    std::unordered_map<std::wstring, std::vector<uint32_t>> _byFirstSyll;  // 首音节 → 词条（残段匹配用）
    std::unordered_map<wchar_t, std::vector<std::wstring>> _syllByFirst;   // 首字母 → 音节（长度降序）
    std::unordered_set<std::wstring> _syllSet;                             // 全部合法音节
};

} // namespace

IPredictor* CreatePinyinPredictor()
{
    return new PinyinEngine();
}

} // namespace lingxi
