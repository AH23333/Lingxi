// pinyin.cpp - M5 拼音引擎：字母串 → 汉字候选。
//
// 核心链路（R1 重构）：
//   1) 音节切分：枚举多条切分路径，每条路径由「音节单元」组成——
//      FULL（完整音节）、INITIAL（单字母简拼）、PARTIAL（末尾残段）。
//      用 DP 每位置保留 top-K 路径，既保留精确切分，也保留「西安」这类
//      多音节切分（xian 单音节 vs xi-an 双音节）。
//   2) 词索引：词的完整音节序列 + 首字母串；紧凑串精确匹配、首字母串
//      纯简拼匹配，混合/残段走「首音节 + 音节数」桶逐音节过滤。
//   3) 词链 Viterbi：每条路径求最优词链（DpSuffix），跨路径汇总，
//      按「覆盖率 + 词频 + 简拼/残段罚分」排序，输出整句与单词。
//
#include "predictor.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../common/log.h"
#include "../../common/settings.h"

namespace lingxi {

namespace {

// ---------------------------------------------------------------------------
// 小鹤双拼 → 全拼（Predict 入口前转换，后续逻辑完全复用全拼）
// 键位依据：shuangpin.xyz 小鹤方案（zh→V ch→I sh→U）。
// ---------------------------------------------------------------------------
std::string SpInitial(char c)
{
    if (c == 'v') return "zh";
    if (c == 'i') return "ch";
    if (c == 'u') return "sh";
    return std::string(1, c);
}

// 韵母键 + 已解析声母 → 全拼韵母；歧义键按声母消解。
std::string SpFinal(char c, const std::string& initial)
{
    auto is = [&](std::initializer_list<const char*> set)
    {
        for (const char* s : set)
            if (initial == s) return true;
        return false;
    };
    switch (c)
    {
    case 'q': return "iu";
    case 'w': return "ei";
    case 'e': return "e";
    case 't': return "ue";
    case 'y': return "un";
    case 'u': return "u";
    case 'i': return "i";
    case 'p': return "ie";
    case 'a': return "a";
    case 'd': return "ai";
    case 'f': return "en";
    case 'g': return "eng";
    case 'h': return "ang";
    case 'j': return "an";
    case 'z': return "ou";
    case 'c': return "ao";
    case 'b': return "in";
    case 'n': return "iao";
    case 'm': return "ian";
    case 'r': return "uan"; // 声母 + R 只能是 uan；er 走零声母
    case 'o': return is({"b", "p", "m", "f"}) ? "o" : "uo";
    case 's': return is({"j", "q", "x"}) ? "iong" : "ong";
    case 'k': return is({"g", "k", "h", "zh", "ch", "sh"}) ? "uai" : "ing";
    case 'l': return is({"g", "k", "h", "zh", "ch", "sh"}) ? "uang" : "iang";
    case 'x': return is({"g", "k", "h", "zh", "ch", "sh"}) ? "ua" : "ia";
    case 'v':
        if (is({"j", "q", "x"})) return "u"; // ju/qu/xu
        if (is({"n", "l"})) return "v";      // nv/lv
        return "ui";
    }
    return "";
}

// 零声母音节 (c1,c2) → 全拼韵母。
bool SpZeroFinal(char c1, char c2, std::string& out)
{
    if (c1 == c2 && (c1 == 'a' || c1 == 'o' || c1 == 'e'))
    { out = std::string(1, c1); return true; } // 啊/哦/额
    const char* pairs[][2] = {
        {"ai", "ai"}, {"ei", "ei"}, {"ao", "ao"}, {"ou", "ou"},
        {"an", "an"}, {"en", "en"}, {"er", "er"}, {"ah", "ang"},
        {"eg", "eng"}};
    std::string key(1, c1);
    key.push_back(c2);
    for (auto& pr : pairs)
        if (key == pr[0]) { out = pr[1]; return true; }
    return false;
}

// 双拼串 → 全拼串；奇数长度时末尾单字符按声母（含零声母首字母）作残段。
std::string ShuangpinToQuanpin(const std::string& sp)
{
    std::string out;
    size_t i = 0;
    for (; i + 1 < sp.size(); i += 2)
    {
        char c1 = sp[i], c2 = sp[i + 1];
        std::string syll;
        if (c1 == 'a' || c1 == 'o' || c1 == 'e')
            SpZeroFinal(c1, c2, syll);
        if (syll.empty())
        {
            std::string initial = SpInitial(c1);
            syll = initial + SpFinal(c2, initial);
        }
        out += syll;
    }
    if (i < sp.size()) out += SpInitial(sp[i]); // 残段
    return out;
}

// ---------------------------------------------------------------------------
// 音节单元与切分路径
// ---------------------------------------------------------------------------
struct SyllUnit
{
    enum Kind : uint8_t { FULL, INITIAL, PARTIAL };
    Kind kind = FULL;
    std::string text; // 小写
};

struct SegPath
{
    std::vector<SyllUnit> units;
    int cost = 0;
};

// ---------------------------------------------------------------------------
// 词条
// ---------------------------------------------------------------------------
struct PyEntry
{
    std::wstring word;
    std::vector<const std::string*> sylls; // 指向音节池中的规范音节
    int rank = 0;
};

struct Candidate
{
    std::wstring word;
    double cost = 0.0;
    int covered = 0; // 覆盖的音节单元数
    int units = 1;   // 该路径的总音节单元数
    int words = 1;   // 词链词数（整词=1，越少越优）
    int tier = 0;    // 切分路径层：0=最自然切分，越大越碎
};

// ---------------------------------------------------------------------------
// 拼音引擎
// ---------------------------------------------------------------------------
class PinyinPredictor : public IPredictor
{
public:
    PinyinPredictor() { Load(); }

    Prediction Predict(const std::wstring& context) override
    {
        Prediction p;
        if (context.empty()) return p;
        Settings st = LoadSettings();
        _fuzzy = st.fuzzyPinyin; // 开关实时生效
        std::string raw = ToUtf8(context);
        if (raw.empty()) return p;
        if (st.shuangpin) raw = ShuangpinToQuanpin(raw); // 双拼→全拼

        std::vector<SegPath> paths = EnumeratePaths(raw);
        if (paths.empty())
        {
            LX_LOGV(L"py", L"input=%hs paths=0", raw.c_str());
            return p;
        }
        for (const auto& pp : paths)
        {
            std::string desc;
            for (const auto& u : pp.units)
            {
                desc += (u.kind == SyllUnit::FULL ? "F:" :
                         u.kind == SyllUnit::INITIAL ? "I:" : "P:");
                desc += u.text;
                desc += " ";
            }
            LX_LOGV(L"py", L"path cost=%d %hs", pp.cost, desc.c_str());
        }

        std::vector<Candidate> out = Generate(paths);
        for (const auto& c : out)
            LX_LOGV(L"py", L"cand cost=%.2f %hs", c.cost, ToUtf8(c.word).c_str());

        // 去重（保序，已按优先级排序）
        std::unordered_set<std::wstring> seen;
        for (auto& c : out)
        {
            if (c.word.empty() || seen.count(c.word)) continue;
            seen.insert(c.word);
            p.cands.push_back(c.word);
            if (p.cands.size() >= kPredictMaxCandidates) break;
        }

        // 自学习置顶：用户在该拼音串下选过的词，按选择次数降序提到最前
        // （即使常规候选未生成，也补入，保证用户偏好可重现）；其余候选保序。
        std::string key = NormKey(raw);
        // 仅在锁内做查找+拷出（微秒级）；排序与重排留在锁外，不占热路径预算。
        std::vector<UserPick> ups;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            if (auto uit = _userDict.find(key); uit != _userDict.end())
                ups = uit->second;
            // 简拼/缩写：当前输入串作为首字母串（如 mm）匹配用户词
            if (auto iit = _userByInit.find(raw); iit != _userByInit.end())
            {
                for (const auto& ip : iit->second)
                {
                    bool dup = false;
                    for (const auto& u : ups)
                        if (u.word == ip.word) { dup = true; break; }
                    if (!dup) ups.push_back({ ip.word, ip.count });
                }
            }
        }
        if (!ups.empty())
        {
            std::stable_sort(ups.begin(), ups.end(),
                             [](const UserPick& a, const UserPick& b) {
                                 if (a.count != b.count) return a.count > b.count;
                                 return a.word < b.word;
                             });
            std::vector<std::wstring> reordered;
            reordered.reserve(p.cands.size() + ups.size());
            std::unordered_set<std::wstring> promoted;
            for (const auto& up : ups)
                if (promoted.insert(up.word).second) reordered.push_back(up.word);
            for (const auto& w : p.cands)
                if (promoted.insert(w).second) reordered.push_back(w);
            if (reordered.size() > kPredictMaxCandidates)
                reordered.resize(kPredictMaxCandidates);
            p.cands = std::move(reordered);
        }

        if (st.traditionalOutput)
            for (auto& w : p.cands) w = ToTraditional(w);

        p.replaceLen = 0;
        return p;
    }

private:
    std::vector<PyEntry> _entries;
    std::unordered_set<std::string> _syllables;

    // 音节字符串池：entries 中重复出现的音节（如"jin"）只存一份，sylls 持指针。
    // deque 保证元素地址稳定（插入不移动已有元素），配合 map 去重。
    std::deque<std::string> _syllPool;
    std::unordered_map<std::string, const std::string*> _syllIntern;
    const std::string* InternSyll(const std::string& s)
    {
        auto it = _syllIntern.find(s);
        if (it != _syllIntern.end()) return it->second;
        _syllPool.push_back(s);
        const std::string* p = &_syllPool.back();
        _syllIntern.emplace(s, p);
        return p;
    }

    // 全拼紧凑键（无分隔，如 "jintian"）→ 词：所有词，全拼查询 O(1) 命中。
    std::unordered_map<std::string, std::vector<int>> _byCompact;
    // 简拼/混合模式键（至少含一个首字母段，如 "-j|-t"、"=jin|-t"）→ 词。
    // 高频词（rank<kFullPatternRank）预生成全部 2^n-1 个非全拼模式；长尾词仅建
    // 全简拼键（长尾几乎只走全拼），控制大词库下的索引体积。
    std::unordered_map<std::string, std::vector<int>> _byPattern;
    // 首音节 → 音节数(1..10) → 词（混合/残段过滤）
    std::unordered_map<std::string, std::vector<std::vector<int>>> _byFirstSyll;
    // 首字母 → 音节数 → 词
    std::unordered_map<std::string, std::vector<std::vector<int>>> _byFirstInit;
    // 单音节 → 单字词（含多个同音字）
    std::unordered_map<std::string, std::vector<int>> _charBySyll;

    // 模糊音开关（每次 Predict 从设置读取，默认开）
    bool _fuzzy = true;
    // 音节 → 合法模糊变体（含自身，自身在首位）；加载后一次性预计算，热路径只查表。
    std::unordered_map<std::string, std::vector<std::string>> _fuzzyOf;

    // 自学习用户词典：规范化拼音串 → 用户选过的词（按次数）。
    // 三方并发访问：管道预测线程（Predict 读）、管道读线程（Learn 写）、
    // UI 线程（设置界面清空/导入后 ReloadUserDict 重载）——全部经 _udMtx 串行化。
    struct UserPick { std::wstring word; int count; };
    std::unordered_map<std::string, std::vector<UserPick>> _userDict;
    // 用户词简拼索引：首字母串（如 "mm"）→ 用户词（含全拼键与次数）
    struct UserInitPick { std::wstring word; int count; std::string fullKey; };
    std::unordered_map<std::string, std::vector<UserInitPick>> _userByInit;
    bool _userDirty = false;
    mutable std::mutex _udMtx;

    // 简体→繁体字符映射（Unicode 码点，正确处理扩展区代理对）
    std::unordered_map<uint32_t, uint32_t> _st2t;

    // -----------------------------------------------------------------------
    // 加载
    // -----------------------------------------------------------------------
    void Load()
    {
        std::string path = ExeDir() + "\\data\\zh_pinyin.txt";
        std::string raw = ReadAll(path);
        if (raw.empty()) return;

        std::vector<std::string> lines = Split(raw, '\n');
        for (auto& line : lines)
        {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.empty()) continue;
            size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;

            std::wstring word = FromUtf8(l.substr(0, tab));
            std::string py = l.substr(tab + 1);
            std::vector<std::string> sylls = Split(py, ' ');
            if (word.empty() || sylls.empty()) continue;

            PyEntry e;
            e.word = word;
            e.rank = (int)_entries.size();
            e.sylls.reserve(sylls.size());
            for (const auto& s : sylls) e.sylls.push_back(InternSyll(s));
            _entries.push_back(std::move(e));
        }

        BuildIndexes();
        BuildSyllableSet();
        BuildFuzzyIndex();
        LoadUserDict();
        LoadTraditionalMap();
    }

    // 加载简体→繁体字符映射（data/st2t.txt，每行「简\t繁」，单字码点）
    void LoadTraditionalMap()
    {
        std::string raw = ReadAll(ExeDir() + "\\data\\st2t.txt");
        if (raw.empty()) return;
        for (auto& line : Split(raw, '\n'))
        {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            std::wstring k = FromUtf8(l.substr(0, tab));
            std::wstring v = FromUtf8(l.substr(tab + 1));
            uint32_t cpK = FirstCodePoint(k);
            uint32_t cpV = FirstCodePoint(v);
            if (cpK != 0xFFFFFFFF && cpV != 0xFFFFFFFF) _st2t[cpK] = cpV;
        }
    }

    // 取 UTF-16 串首个 Unicode 码点（处理代理对）；空串返回 0xFFFFFFFF
    static uint32_t FirstCodePoint(const std::wstring& s)
    {
        if (s.empty()) return 0xFFFFFFFF;
        uint32_t c = (uint32_t)(uint16_t)s[0];
        if (c >= 0xD800 && c <= 0xDBFF && s.size() >= 2)
        {
            uint32_t lo = (uint32_t)(uint16_t)s[1];
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        return c;
    }

    // 简体串→繁体串：逐码点查表，无映射则保留原字
    std::wstring ToTraditional(const std::wstring& s)
    {
        if (_st2t.empty()) return s;
        std::wstring out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();)
        {
            uint32_t cp = (uint32_t)(uint16_t)s[i];
            size_t step = 1;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size())
            {
                uint32_t lo = (uint32_t)(uint16_t)s[i + 1];
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                step = 2;
            }
            auto it = _st2t.find(cp);
            uint32_t mapped = (it != _st2t.end()) ? it->second : cp;
            if (mapped > 0xFFFF)
            {
                mapped -= 0x10000;
                out.push_back((wchar_t)(0xD800 + (mapped >> 10)));
                out.push_back((wchar_t)(0xDC00 + (mapped & 0x3FF)));
            }
            else
            {
                out.push_back((wchar_t)mapped);
            }
            i += step;
        }
        return out;
    }

    void BuildIndexes()
    {
        // 预留容量，避免逐词插入时反复 rehash（降低内存峰值与加载时间）。
        size_t M = _entries.size();
        _byCompact.reserve(M + 64);
        _byPattern.reserve(M * 3 / 4 + 64);
        _byFirstSyll.reserve(512);
        _byFirstInit.reserve(64);
        _syllIntern.reserve(512);

        for (int i = 0; i < (int)_entries.size(); i++)
        {
            const PyEntry& e = _entries[i];
            if (e.sylls.empty()) continue;
            int n = (int)e.sylls.size();

            // 全拼紧凑键（无分隔，如 "jintian"）：所有词都建，全拼 O(1) 命中。
            std::string compact;
            for (const std::string* ps : e.sylls) compact += *ps;
            _byCompact[compact].push_back(i);

            // 按 mask 构造混合模式键：bit=1 取全拼(=syll)，bit=0 取首字母(-init)。
            auto makePatternKey = [&](int mask) {
                std::string key;
                key.reserve(compact.size() + (size_t)n * 2);
                for (int k = 0; k < n; k++)
                {
                    if (k) key.push_back('|');
                    const std::string& sy = *e.sylls[k];
                    if (mask & (1 << k)) { key.push_back('='); key += sy; }
                    else { key.push_back('-'); key.push_back(sy[0]); }
                }
                return key;
            };

            // 简拼/混合模式（仅 n<=6，2^n 有界）：
            //   高频词：除全拼外的全部 2^n-1 个混合模式（含全简拼 mask=0）；
            //   长尾词：仅全简拼键（长尾几乎只走全拼，混合简拼需求极低）。
            if (n >= 1 && n <= 6)
            {
                if (i < kFullPatternRank)
                {
                    for (int mask = 0; mask < (1 << n) - 1; mask++)
                        _byPattern[makePatternKey(mask)].push_back(i);
                }
                else
                {
                    _byPattern[makePatternKey(0)].push_back(i);
                }
            }

            // 单字
            if (e.word.size() == 1 && n == 1)
                _charBySyll[*e.sylls[0]].push_back(i);

            if (n >= 1 && n <= 10)
            {
                EnsureSyllBucket(_byFirstSyll[*e.sylls[0]], n).push_back(i);
                std::string init(1, (*e.sylls[0])[0]);
                EnsureSyllBucket(_byFirstInit[init], n).push_back(i);
            }
        }

        // 同键词按 rank 升序（加载即按词频序，保持稳定）
        for (auto& kv : _byCompact) StableSortIdx(kv.second);
        for (auto& kv : _byPattern) StableSortIdx(kv.second);
        for (auto& kv : _charBySyll) StableSortIdx(kv.second);
    }

    static std::vector<int>& EnsureSyllBucket(std::vector<std::vector<int>>& v, int n)
    {
        if ((int)v.size() <= n) v.resize(n + 1);
        return v[n];
    }
    void StableSortIdx(std::vector<int>& v)
    {
        std::stable_sort(v.begin(), v.end(),
                         [&](int a, int b) { return _entries[a].rank < _entries[b].rank; });
    }

    // 合法音节集合：数据中出现的单音节 + 常见音节兜底
    void BuildSyllableSet()
    {
        for (const auto& e : _entries)
            for (const std::string* s : e.sylls)
                if (s->size() <= 6) _syllables.insert(*s);
        static const char* kExtra[] = {
            "a","ai","an","ang","ao","ba","bai","ban","bang","bao","bei","ben","beng","bi",
            "bian","biao","bie","bin","bing","bo","bu","ca","cai","can","cang","cao","ce",
            "cen","ceng","cha","chai","chan","chang","chao","che","chen","cheng","chi","chong",
            "chou","chu","chua","chuai","chuan","chuang","chui","chun","chuo","ci","cong","cou",
            "cu","cuan","cui","cun","cuo","da","dai","dan","dang","dao","de","dei","den","deng",
            "di","dia","dian","diao","die","ding","diu","dong","dou","du","duan","dui","dun",
            "duo","e","ei","en","er","fa","fan","fang","fei","fen","feng","fo","fou","fu","ga",
            "gai","gan","gang","gao","ge","gei","gen","geng","gong","gou","gu","gua","guai",
            "guan","guang","gui","gun","guo","ha","hai","han","hang","hao","he","hei","hen",
            "heng","hm","hong","hou","hu","hua","huai","huan","huang","hui","hun","huo","ji",
            "jia","jian","jiang","jiao","jie","jin","jing","jiong","jiu","ju","juan","jue",
            "jun","ka","kai","kan","kang","kao","ke","ken","keng","kong","kou","ku","kua",
            "kuai","kuan","kuang","kui","kun","kuo","la","lai","lan","lang","lao","le","lei",
            "leng","li","lia","lian","liang","liao","lie","lin","ling","liu","long","lou","lu",
            "lv","luan","lve","lun","luo","ma","mai","man","mang","mao","me","mei","men",
            "meng","mi","mian","miao","mie","min","ming","miu","mo","mou","mu","na","nai",
            "nan","nang","nao","ne","nei","nen","neng","ni","nian","niang","niao","nie","nin",
            "ning","niu","nong","nou","nu","nv","nuan","nve","nun","nuo","o","ou","pa","pai",
            "pan","pang","pao","pei","pen","peng","pi","pian","piao","pie","pin","ping","po",
            "pou","pu","qi","qia","qian","qiang","qiao","qie","qin","qing","qiong","qiu","qu",
            "quan","que","qun","ran","rang","rao","re","ren","reng","ri","rong","rou","ru",
            "rua","ruan","rui","run","ruo","sa","sai","san","sang","sao","se","sen","seng",
            "sha","shai","shan","shang","shao","she","shei","shen","sheng","shi","shou","shu",
            "shua","shuai","shan","shuang","shui","shun","shuo","si","song","sou","su","suan",
            "sui","sun","suo","ta","tai","tan","tang","tao","te","teng","ti","tian","tiao",
            "tie","ting","tong","tou","tu","tuan","tui","tun","tuo","wa","wai","wan","wang",
            "wei","wen","weng","wo","wu","xi","xia","xian","xiang","xiao","xie","xin","xing",
            "xiong","xiu","xu","xuan","xue","xun","ya","yan","yang","yao","ye","yi","yin",
            "ying","yo","yong","you","yu","yuan","yue","yun","za","zai","zan","zang","zao",
            "ze","zei","zen","zeng","zha","zhai","zhan","zhang","zhao","zhe","zhei","zhen",
            "zheng","zhi","zhong","zhou","zhu","zhua","zhuai","zhuan","zhuang","zhui","zhun",
            "zhuo","zi","zong","zou","zu","zuan","zui","zun","zuo"
        };
        for (auto* s : kExtra) _syllables.insert(s);
    }

    // -----------------------------------------------------------------------
    // 音节切分
    // -----------------------------------------------------------------------
    static constexpr int K_POS = 7;   // 每位置保留路径数
    static constexpr int K_FINAL = 14; // 最终参与候选的路径数
    // rank 小于此值的高频词预生成全部混合简拼模式；长尾词仅全拼+全简拼。
    static constexpr int kFullPatternRank = 50000;

    std::vector<SegPath> EnumeratePaths(const std::string& rawIn)
    {
        // 规范化：仅保留 a-z；撇号作为边界（空转移）
        std::string in;
        in.reserve(rawIn.size());
        for (char ch : rawIn)
        {
            char c = Lower(ch);
            if (c >= 'a' && c <= 'z') in.push_back(c);
            else if (ch == '\'' || ch == '\u2019') in.push_back('\'');
        }
        int n = (int)in.size();

        std::vector<std::vector<SegPath>> dp(n + 1);
        dp[0].push_back(SegPath());

        for (int i = 0; i < n; i++)
        {
            if (i + 1 <= n && in[i] == '\'')
            {
                // 边界：路径原样前推
                dp[i + 1] = dp[i];
                Prune(dp[i + 1]);
                continue;
            }
            if (dp[i].empty()) continue;

            // 枚举从 i 开始的完整音节
            int bound = NextBoundaryOrEnd(in, i);
            for (int L = 1; L <= 6 && i + L <= bound; L++)
            {
                std::string s = in.substr(i, L);
                if (!_syllables.count(s)) continue;
                Extend(dp, i, i + L, SyllUnit{ SyllUnit::FULL, s }, 2);
            }
            // 单字母简拼
            if (in[i] >= 'a' && in[i] <= 'z')
                Extend(dp, i, i + 1, SyllUnit{ SyllUnit::INITIAL, std::string(1, in[i]) }, 4);
        }

        // 收集完整路径 + 末尾残段路径
        std::vector<SegPath> all;
        if (!dp[n].empty())
            for (const auto& p : dp[n]) all.push_back(p);

        for (int i = 0; i < n; i++)
        {
            if (dp[i].empty()) continue;
            int b = NextBoundaryOrEnd(in, i);
            int rlen = b - i;
            if (rlen <= 0 || rlen > 6) continue;
            std::string r = in.substr(i, rlen);
            if (r.empty() || r[0] == '\'') continue;
            if (_syllables.count(r)) continue;       // 完整音节走完整路径
            if (!IsSyllablePrefix(r)) continue;       // 必须是某音节前缀
            for (const auto& p : dp[i])
            {
                SegPath np = p;
                np.units.push_back(SyllUnit{ SyllUnit::PARTIAL, r });
                np.cost += 3;
                all.push_back(np);
            }
        }

        Prune(all);
        if ((int)all.size() > K_FINAL) all.resize(K_FINAL);
        return all;
    }

    void Extend(std::vector<std::vector<SegPath>>& dp, int from, int to,
                SyllUnit u, int addCost)
    {
        for (const auto& p : dp[from])
        {
            SegPath np = p;
            np.units.push_back(u);
            np.cost += addCost;
            dp[to].push_back(np);
        }
        Prune(dp[to]);
    }

    static int NextBoundaryOrEnd(const std::string& in, int i)
    {
        for (int j = i; j < (int)in.size(); j++)
            if (in[j] == '\'') return j;
        return (int)in.size();
    }

    bool IsSyllablePrefix(const std::string& r) const
    {
        for (const auto& s : _syllables)
            if (s.size() >= r.size() && s.compare(0, r.size(), r) == 0) return true;
        return false;
    }

    // 声母长度：zh/ch/sh 为 2，其余单字母声母为 1，零声母(a/o/e…) 为 0。
    static int InitialLen(const std::string& s)
    {
        if (s.size() >= 2 && (s[0] == 'z' || s[0] == 'c' || s[0] == 's') && s[1] == 'h')
            return 2;
        if (s.empty()) return 0;
        switch (s[0])
        {
        case 'b': case 'p': case 'm': case 'f': case 'd': case 't': case 'n':
        case 'l': case 'g': case 'k': case 'h': case 'j': case 'q': case 'x':
        case 'r': case 'z': case 'c': case 's': case 'y': case 'w':
            return 1;
        }
        return 0;
    }

    // 一次性预计算每个合法音节的模糊变体缓存（热路径只查表，不再重复枚举）。
    void BuildFuzzyIndex()
    {
        _fuzzyOf.clear();
        for (const auto& s : _syllables)
            _fuzzyOf.emplace(s, ComputeFuzzy(s));
    }

    // -----------------------------------------------------------------------
    // 自学习用户词典（%APPDATA%\LingxiIme\userdict.txt，UTF-8）
    // -----------------------------------------------------------------------
    static std::string UserDictPath()
    {
        char buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::string dir = std::string(buf) + "\\LingxiIme";
            CreateDirectoryA(dir.c_str(), NULL);
            return dir + "\\userdict.txt";
        }
        return ExeDir() + "\\userdict.txt";
    }

    // 学习键：只保留小写字母（与音节切分规范化一致，空格/分隔符丢弃）。
    static std::string NormKey(const std::string& raw)
    {
        std::string k;
        k.reserve(raw.size());
        for (char ch : raw)
        {
            char c = Lower(ch);
            if (c >= 'a' && c <= 'z') k.push_back(c);
        }
        return k;
    }

    void LoadUserDict()
    {
        std::lock_guard<std::mutex> lk(_udMtx);
        _userDict.clear();
        std::string raw = ReadAll(UserDictPath());
        if (raw.empty()) return;
        for (auto& line : Split(raw, '\n'))
        {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            size_t t1 = l.find('\t');
            if (t1 == std::string::npos) continue;
            size_t t2 = l.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::wstring word = FromUtf8(l.substr(0, t1));
            std::string key = l.substr(t1 + 1, t2 - t1 - 1);
            int count = std::atoi(l.substr(t2 + 1).c_str());
            if (!word.empty() && !key.empty() && count > 0)
                _userDict[key].push_back({ word, count });
        }
        BuildUserInitIndex();
        LX_LOG(L"py", L"用户词典加载: %zu 个拼音键, %zu 个简拼键",
               _userDict.size(), _userByInit.size());
    }

    // 构建用户词简拼索引（调用方持 _udMtx）：全拼键切出音节，取首字母串
    void BuildUserInitIndex()
    {
        _userByInit.clear();
        for (const auto& kv : _userDict)
        {
            const std::string& fullKey = kv.first;
            std::vector<SegPath> sp = EnumeratePaths(fullKey);
            if (sp.empty()) continue;
            const SegPath* best = &sp[0]; // 最自然切分（cost 最小）
            for (const auto& p : sp)
                if (p.cost < best->cost) best = &p;
            std::string init;
            for (const auto& u : best->units)
                if (!u.text.empty()) init.push_back(u.text[0]);
            if (init.empty()) continue;
            auto& vec = _userByInit[init];
            for (const auto& up : kv.second)
                vec.push_back({ up.word, up.count, fullKey });
        }
    }

    // 原子落盘：先写 .tmp 再 MoveFileEx 替换。引擎（管道线程）与设置界面
    // （UI 线程）是同一文件的两个写者，任何一方都不允许让对方读到半截文件。
    static bool WriteFileAtomicA(const std::string& path, const std::string& content)
    {
        std::string tmp = path + ".tmp";
        HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, NULL);
        CloseHandle(h);
        if (!ok || written != (DWORD)content.size())
        {
            DeleteFileA(tmp.c_str());
            return false;
        }
        if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileA(tmp.c_str());
            return false;
        }
        return true;
    }

    // 落盘（调用方必须已持有 _udMtx）。失败保持 _userDirty，下次学习时重试。
    bool SaveUserDictLocked()
    {
        if (!_userDirty) return true;
        std::string content;
        for (const auto& kv : _userDict)
            for (const auto& up : kv.second)
            {
                content += ToUtf8(up.word);
                content.push_back('\t');
                content += kv.first;
                content.push_back('\t');
                content += std::to_string(up.count);
                content.push_back('\n');
            }
        if (!WriteFileAtomicA(UserDictPath(), content)) return false;
        _userDirty = false;
        return true;
    }

    void SaveUserDict()
    {
        std::lock_guard<std::mutex> lk(_udMtx);
        SaveUserDictLocked();
    }

    // 用户选择反馈：该拼音串下，被选词计数 +1，落盘后下次同串置顶。
    void Learn(const std::wstring& rawPinyin, const std::wstring& word) override
    {
        if (rawPinyin.empty() || word.empty()) return;
        std::string key = NormKey(ToUtf8(rawPinyin));
        if (key.empty()) return;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            auto& vec = _userDict[key];
            bool found = false;
            for (auto& up : vec)
                if (up.word == word) { up.count++; found = true; break; }
            if (!found)
            {
                if (vec.size() >= 8) vec.pop_back(); // 每键最多 8 个偏好，控制体积
                vec.push_back({ word, 1 });
            }
            _userDirty = true;
            SaveUserDictLocked();
            BuildUserInitIndex(); // 新词即时可通过简拼调出
        }
        LX_LOG(L"py", L"学习: '%hs' -> '%hs'", key.c_str(), ToUtf8(word).c_str());
    }

    // 管理界面清空/导入后重新加载用户词典，立即生效。
    void ReloadUserDict() override { LoadUserDict(); }

    // 模糊音：返回该完整音节的合法等价音节（自身在首位）。
    // 声母 z/zh、c/ch、s/sh、n/l 互通；韵母前/后鼻音尾 n/ng 互换；
    // 仅保留 _syllables 中真实存在的音节（自动剔除 sin、ung 这类非法组合）。
    std::vector<std::string> ComputeFuzzy(const std::string& s) const
    {
        std::vector<std::string> out;
        out.push_back(s);
        int iLen = InitialLen(s);
        std::string initial = s.substr(0, iLen);
        std::string final = s.substr(iLen);

        std::vector<std::string> initVar{ initial };
        if (initial == "z") initVar.push_back("zh");
        else if (initial == "zh") initVar.push_back("z");
        else if (initial == "c") initVar.push_back("ch");
        else if (initial == "ch") initVar.push_back("c");
        else if (initial == "s") initVar.push_back("sh");
        else if (initial == "sh") initVar.push_back("s");
        else if (initial == "n") initVar.push_back("l");
        else if (initial == "l") initVar.push_back("n");

        std::vector<std::string> finVar{ final };
        if (final.size() >= 2 && final[final.size() - 1] == 'g' &&
            final[final.size() - 2] == 'n')
        {
            finVar.push_back(final.substr(0, final.size() - 1)); // -ng → -n
        }
        else if (!final.empty() && final.back() == 'n')
        {
            finVar.push_back(final + "g"); // -n → -ng
        }

        for (const auto& ini : initVar)
            for (const auto& fin : finVar)
            {
                std::string cand = ini + fin;
                if (cand != s && _syllables.count(cand) &&
                    std::find(out.begin(), out.end(), cand) == out.end())
                    out.push_back(cand);
            }
        return out;
    }

    static void Prune(std::vector<SegPath>& v)
    {
        // 去重（相同单元序列）
        std::unordered_set<std::string> keys;
        std::vector<SegPath> uniq;
        uniq.reserve(v.size());
        for (auto& p : v)
        {
            std::string k;
            for (auto& u : p.units)
            {
                k.push_back((char)u.kind);
                k += u.text;
                k.push_back('|');
            }
            if (keys.insert(k).second) uniq.push_back(p);
        }
        std::sort(uniq.begin(), uniq.end(),
                  [](const SegPath& a, const SegPath& b) { return a.cost < b.cost; });
        if ((int)uniq.size() > K_POS) uniq.resize(K_POS);
        v.swap(uniq);
    }

    // -----------------------------------------------------------------------
    // 词匹配
    // -----------------------------------------------------------------------
    struct WordMatch
    {
        int idx;
        double penalty;
    };

    // 词的 penalty：简拼在单字词中重罚（简拼通常为打词而非打单字），
    // 残段轻罚。
    double PenaltyFor(const std::vector<SyllUnit>& units, bool singleChar) const
    {
        double pen = 0.0;
        for (const auto& u : units)
        {
            if (u.kind == SyllUnit::INITIAL) pen += singleChar ? 4.0 : 1.5;
            else if (u.kind == SyllUnit::PARTIAL) pen += 2.0;
        }
        return pen;
    }
    // 词的单项成本：按加载行号（词频序）取对数，越常用成本越低。
    double WordCost(int idx) const
    {
        return std::log(1.0 + (double)_entries[idx].rank);
    }

    // 词数成本：log(1+rank) 已符合 Zipf 单字概率模型，多字词的 rank 本身
    // 反映其概率，不再额外奖励"少词"——否则生僻长词（相去、石碣镇）会靠
    // 少一个词数罚压过高频单字组合（想+去、世界+真美）。
    static constexpr double kWordCountCost = 0.0;
    // 每个模糊音节的罚分：使模糊命中排在精确命中之后，但仍可作为容错候选。
    static constexpr double K_FUZZY = 3.0;
    // 模糊音仅对至多 3 音节的词做变体枚举（控制组合数 4^3 以内）。
    static constexpr int K_FUZZY_MAX_SYLL = 3;

    std::vector<WordMatch> MatchWords(const std::vector<SyllUnit>& units)
    {
        std::vector<WordMatch> out;
        if (units.empty()) return out;
        int n = (int)units.size();
        if (n > 10) return out;

        auto add = [&](int ei) {
            bool single = _entries[ei].word.size() == 1;
            out.push_back({ ei, PenaltyFor(units, single) });
        };

        bool hasPartial = false;
        for (const auto& u : units)
            if (u.kind == SyllUnit::PARTIAL) { hasPartial = true; break; }
        bool appendedFuzzy = false; // 是否追加了模糊命中（仅此时才需要重排）

        if (!hasPartial)
        {
            // 精确匹配（O(1)）：全 FULL 查紧凑全拼索引；含简拼查混合模式索引。
            bool allFull = true;
            std::string compact;
            std::string pattern;
            compact.reserve(24);
            pattern.reserve(24);
            for (int k = 0; k < n; k++)
            {
                const std::string& t = units[k].text;
                if (t.empty()) return out;
                if (units[k].kind == SyllUnit::FULL)
                {
                    compact += t;
                    if (k) pattern.push_back('|');
                    pattern.push_back('='); pattern += t;
                }
                else
                {
                    allFull = false;
                    if (k) pattern.push_back('|');
                    pattern.push_back('-'); pattern.push_back(t[0]);
                }
            }
            std::unordered_set<int> exactEi;
            if (allFull)
            {
                auto it = _byCompact.find(compact);
                if (it != _byCompact.end())
                    for (int i : it->second) { add(i); exactEi.insert(i); }
            }
            else
            {
                auto it = _byPattern.find(pattern);
                if (it != _byPattern.end())
                    for (int i : it->second) { add(i); exactEi.insert(i); }
            }

            // 模糊音：全 FULL、短词（≤3 音节）。枚举每个音节的声母/韵母合法
            // 变体组合，查紧凑全拼索引；命中词按“模糊音节数”加罚，精确命中优先。
            if (_fuzzy && allFull && n <= K_FUZZY_MAX_SYLL)
            {
                std::vector<std::vector<std::string>> var(n);
                bool anyFuzzy = false;
                for (int k = 0; k < n; k++)
                {
                    auto fit = _fuzzyOf.find(units[k].text);
                    if (fit != _fuzzyOf.end()) var[k] = fit->second;
                    else var[k] = { units[k].text };
                    if (var[k].size() > 1) anyFuzzy = true;
                }
                if (anyFuzzy)
                {
                    std::unordered_map<int, int> bestFuzzy; // ei → 最小模糊音节数
                    size_t total = 1;
                    for (auto& v : var) total *= v.size();
                    for (size_t comb = 1; comb < total; comb++) // comb=0 即全自身（精确）
                    {
                        size_t t = comb;
                        std::string fk; // 模糊紧凑全拼
                        fk.reserve(24);
                        int fuzzyCount = 0;
                        for (int k = 0; k < n; k++)
                        {
                            size_t pick = t % var[k].size();
                            t /= var[k].size();
                            const std::string& syl = var[k][pick];
                            if (syl != units[k].text) fuzzyCount++;
                            fk += syl;
                        }
                        if (fuzzyCount == 0) continue;
                        auto fit = _byCompact.find(fk);
                        if (fit == _byCompact.end()) continue;
                        for (int ei : fit->second)
                        {
                            if (exactEi.count(ei)) continue;
                            auto bf = bestFuzzy.find(ei);
                            if (bf == bestFuzzy.end() || fuzzyCount < bf->second)
                                bestFuzzy[ei] = fuzzyCount;
                        }
                    }
                    for (const auto& kv : bestFuzzy)
                    {
                        bool single = _entries[kv.first].word.size() == 1;
                        double pen = PenaltyFor(units, single) + kv.second * K_FUZZY;
                        out.push_back({ kv.first, pen });
                    }
                    appendedFuzzy = !bestFuzzy.empty();
                }
            }
        }
        else
        {
        // 含末尾 PARTIAL：首单元桶 + 逐音节过滤（仅残段路径触发，量小）
        const SyllUnit& u0 = units[0];
        if (u0.kind == SyllUnit::PARTIAL) return out; // 整串即残段：走残段单字
        std::vector<int> bucket;
        if (u0.kind == SyllUnit::FULL)
        {
            auto it = _byFirstSyll.find(u0.text);
            if (it != _byFirstSyll.end() && (int)it->second.size() > n)
                bucket = it->second[n];
        }
        else
        {
            auto it = _byFirstInit.find(u0.text);
            if (it != _byFirstInit.end() && (int)it->second.size() > n)
                bucket = it->second[n];
        }
        for (int ei : bucket)
        {
            const PyEntry& e = _entries[ei];
            if ((int)e.sylls.size() != n) continue;
            bool ok = true;
            for (int k = 1; k < n; k++)
            {
                const SyllUnit& u = units[k];
                const std::string& s = *e.sylls[k];
                if (u.kind == SyllUnit::FULL)
                {
                    if (s != u.text) { ok = false; break; }
                }
                else if (u.kind == SyllUnit::INITIAL)
                {
                    if (s.empty() || s[0] != u.text[0]) { ok = false; break; }
                }
                else // PARTIAL（只在末尾）
                {
                    if (s.size() < u.text.size() ||
                        s.compare(0, u.text.size(), u.text) != 0) { ok = false; break; }
                }
            }
            if (ok) add(ei);
        }
        }

        // 仅在追加了模糊命中时重排：精确桶本已按词频(rank)有序，无需重复排序；
        // 混入模糊词后按“词频 + 模糊/简拼罚分”排序，使高频精确词与高频模糊词都
        // 靠前，上层“候选上限”截断时保留的是高价值词，而非随机/生僻词。
        if (appendedFuzzy)
        {
            std::stable_sort(out.begin(), out.end(),
                             [&](const WordMatch& a, const WordMatch& b) {
                                 double ca = WordCost(a.idx) + a.penalty;
                                 double cb = WordCost(b.idx) + b.penalty;
                                 if (ca != cb) return ca < cb;
                                 return a.idx < b.idx;
                             });
        }
        return out;
    }

    // 残段前缀 → 单字（同音字按词频）
    std::vector<int> MatchCharsByPrefix(const std::string& prefix)
    {
        std::vector<int> out;
        for (const auto& kv : _charBySyll)
        {
            const std::string& s = kv.first;
            if (s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0)
                for (int i : kv.second) out.push_back(i);
        }
        std::stable_sort(out.begin(), out.end(),
                         [&](int a, int b) { return _entries[a].rank < _entries[b].rank; });
        return out;
    }

    // -----------------------------------------------------------------------
    // 词链 Viterbi
    // -----------------------------------------------------------------------
    struct Node
    {
        bool valid = false;
        double cost = 0.0;
        int firstIdx = -1;
        int firstSpan = 0;
        int nWords = 0;
    };

    // -----------------------------------------------------------------------
    // 候选生成
    // -----------------------------------------------------------------------
    std::vector<Candidate> Generate(const std::vector<SegPath>& paths)
    {
        std::vector<Candidate> all;
        // 最自然切分（最长音节、最少简拼）的路径 cost 作为 tier 0 基准；
        // 其余碎切分路径（如 nihao 拆成 ni-ha-o）整体降一层，避免其完整候选
        // （"你好奥"之类）挤掉最优路径的单字备选。
        int minPathCost = 1000000;
        for (const auto& p : paths) minPathCost = (std::min)(minPathCost, p.cost);

        for (const auto& path : paths)
        {
            const std::vector<SyllUnit>& units = path.units;
            int n = (int)units.size();
            if (n == 0) continue;

            bool trailingPartial = units.back().kind == SyllUnit::PARTIAL;
            int tier = (path.cost <= minPathCost) ? 0 : 1;
            // 纯声母简拼路径（如 dh、xyzxyz）：简拼价值在于命中整词（dh→电话）。
            // 长串纯声母若只能逐字单字硬拼（xyzxyz），视为英文/误触，不产出中文。
            bool allInit = true;
            for (const auto& u : units)
                if (u.kind != SyllUnit::INITIAL) { allInit = false; break; }

            // 纯声母简拼过长（≥4 个声母，或残段前缀≥3 个声母）几乎必为英文/误触，
            // 即使能撞上罕见长词也整条丢弃（同时省掉该路径的 DP）。
            if (allInit && n >= 4) continue;
            int initHead = trailingPartial ? n - 1 : n;
            if (trailingPartial)
            {
                bool prefixAllInit = true;
                for (int k = 0; k < n - 1; k++)
                    if (units[k].kind != SyllUnit::INITIAL) { prefixAllInit = false; break; }
                if (prefixAllInit && initHead >= 3) continue;
            }

            // 收集候选并记录覆盖单元数、词数与切分层，供分层排序。
            auto add = [&](std::wstring word, double cost, int covered, int words)
            {
                if (!word.empty())
                    all.push_back({ std::move(word), cost, covered, n, words, tier });
            };

            // 匹配缓存：每条路径内 (i,span) 只做一次 MatchWords，避免 O(n^3) 重复
            std::vector<std::vector<std::vector<WordMatch>>> mw(
                n + 1, std::vector<std::vector<WordMatch>>(7));
            std::vector<std::vector<unsigned char>> mwDone(
                n + 1, std::vector<unsigned char>(7, 0));
            auto getMatches = [&](int i, int span) -> const std::vector<WordMatch>& {
                if (!mwDone[i][span])
                {
                    mwDone[i][span] = 1;
                    std::vector<SyllUnit> sub(units.begin() + i, units.begin() + i + span);
                    mw[i][span] = MatchWords(sub);
                }
                return mw[i][span];
            };

            // 一次反向 DP：d[i] = units[i..end) 的最优词链（结果可被回溯复用）
            auto computeDp = [&](int end) {
                std::vector<Node> d(end + 1);
                d[end].valid = true;
                for (int i = end - 1; i >= 0; i--)
                {
                    Node best;
                    for (int span = 1; span <= 6 && i + span <= end; span++)
                    {
                        const std::vector<WordMatch>& ws = getMatches(i, span);
                        if (ws.empty()) continue;
                        const Node& nxt = d[i + span];
                        if (!nxt.valid) continue;
                        for (const auto& w : ws)
                        {
                            double c = WordCost(w.idx) + w.penalty + kWordCountCost + nxt.cost;
                            if (!best.valid || c < best.cost)
                                best = Node{ true, c, w.idx, span, 1 + nxt.nWords };
                        }
                    }
                    d[i] = best;
                }
                return d;
            };
            // 沿已算好的 DP 回溯成词，O(词数)，不重复计算
            auto reconstruct = [&](const std::vector<Node>& d, int end, int from) {
                std::wstring s;
                int i = from;
                while (i < end)
                {
                    const Node& nd = d[i];
                    if (!nd.valid || nd.firstIdx < 0) break;
                    s += _entries[nd.firstIdx].word;
                    i += nd.firstSpan;
                }
                return s;
            };

            std::vector<Node> dp = computeDp(n);

            // 1) 整句最优链（覆盖全部 units）。纯声母简拼只接受"一个整词覆盖全部"，
            // 不做逐字声母硬拼（重码率极高，且 xyzxyz 之类应判为英文）。
            if (dp[0].valid && dp[0].firstIdx >= 0 &&
                !(allInit && dp[0].nWords != 1))
                add(reconstruct(dp, n, 0), dp[0].cost, n, dp[0].nWords);

            // 2) 首词不同的整句变体（用于给出同拼音的其他整词）
            for (int span = 1; span <= 4 && span <= n; span++)
            {
                const std::vector<WordMatch>& ws = getMatches(0, span);
                int added = 0;
                for (const auto& w : ws)
                {
                    if (added >= 3) break;
                    if (w.idx == dp[0].firstIdx && span == dp[0].firstSpan) continue;
                    if (span < n && !dp[span].valid) continue;
                    std::wstring s = _entries[w.idx].word;
                    double c = WordCost(w.idx) + w.penalty + kWordCountCost;
                    int nwords = 1;
                    if (span < n)
                    {
                        s += reconstruct(dp, n, span);
                        c += dp[span].cost;
                        nwords += dp[span].nWords;
                    }
                    if (allInit && nwords != 1) continue; // 纯简拼只给整词，不拼尾巴
                    add(s, c, n, nwords);
                    added++;
                }
            }

            // 3) 单词 / 短语（从位置 0 起，覆盖前若干单元；未覆盖计罚分）。
            // 纯简拼只保留覆盖全部或只差 1 个声母的前缀词（如 d→的、dh→电话）。
            // 完整覆盖输入的词（span==n）放宽到 60 个：单字同音/模糊字、精确与模糊
            // 整词都应可翻页看到；仅覆盖前缀的碎词仍限 4 个，避免噪声。
            for (int span = 1; span <= 4 && span <= n; span++)
            {
                if (allInit && n - span > 1) continue;
                const std::vector<WordMatch>& ws = getMatches(0, span);
                int added = 0;
                int limit = (span == n) ? (int)kPredictMaxCandidates : 4;
                for (const auto& w : ws)
                {
                    if (added >= limit) break;
                    double uncovered = (double)(n - span) * 8.0;
                    double c = WordCost(w.idx) + w.penalty + kWordCountCost + uncovered;
                    add(_entries[w.idx].word, c, span, 1);
                    added++;
                }
            }

            // 4) 末尾残段单字（头部 DP 只覆盖到 n-1，残段单独走前缀单字）
            if (trailingPartial && n >= 1)
            {
                std::string prefix = units.back().text;
                int hEnd = n - 1;
                // 头部是否纯声母简拼（如 xyzxy + 残段 z）
                bool prefixAllInit = true;
                for (int k = 0; k < hEnd; k++)
                    if (units[k].kind != SyllUnit::INITIAL) { prefixAllInit = false; break; }
                std::wstring head;
                double headCost = 0.0;
                int headWords = 0;
                bool headOk = true;
                if (hEnd > 0)
                {
                    std::vector<Node> hdp = computeDp(hEnd);
                    if (hdp[0].valid)
                    {
                        head = reconstruct(hdp, hEnd, 0);
                        headCost = hdp[0].cost;
                        headWords = hdp[0].nWords;
                    }
                    // 纯声母简拼头部（≥2 个声母）：必须由一个整词覆盖，
                    // 否则属逐字硬拼（xyzxyz 这类），整体不输出。
                    if (prefixAllInit && hEnd >= 2 &&
                        !(hdp[0].valid && hdp[0].nWords == 1))
                        headOk = false;
                }
                if (headOk)
                {
                    std::vector<int> chars = MatchCharsByPrefix(prefix);
                    int added = 0;
                    for (int ci : chars)
                    {
                        if (added >= 6) break;
                        double c = headCost + WordCost(ci) + 2.0 + kWordCountCost;
                        add(head + _entries[ci].word, c, n, headWords + 1);
                        added++;
                    }
                }
            }
        }

        // 分层排序：① 切分路径自然度（tier）→ ② 完整覆盖优先（未覆盖单元少）
        // → ③ 概率 cost 低。
        // 不再把"词数少"作为 cost 之前的硬排序：log(1+rank) 已符合 Zipf 词概率
        // 模型，常用整词的 rank 远小于其拆字单字 rank 乘积，自然胜出；而生僻长词
        // （相去、石碣镇）若硬性按词数少优先，会压过更合理的高频词组合。
        std::stable_sort(all.begin(), all.end(),
                         [](const Candidate& a, const Candidate& b)
                         {
                             if (a.tier != b.tier) return a.tier < b.tier;
                             int ua = a.units - a.covered, ub = b.units - b.covered;
                             if (ua != ub) return ua < ub;
                             return a.cost < b.cost;
                         });
        return all;
    }

    // -----------------------------------------------------------------------
    // 工具
    // -----------------------------------------------------------------------
    static std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= s.size())
        {
            size_t f = s.find(sep, pos);
            if (f == std::string::npos) { out.push_back(s.substr(pos)); break; }
            out.push_back(s.substr(pos, f - pos));
            pos = f + 1;
        }
        return out;
    }
    static char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }

    static std::string ExeDir()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        size_t slash = p.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    }
    static std::string ReadAll(const std::string& path)
    {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return {};
        DWORD size = GetFileSize(h, NULL);
        std::string s(size, '\0');
        DWORD read = 0;
        ReadFile(h, s.data(), size, &read, NULL);
        CloseHandle(h);
        s.resize(read);
        return s;
    }

    static std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, NULL, NULL);
        return s;
    }
    static std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }
};

} // namespace

IPredictor* CreatePinyinPredictor()
{
    return new PinyinPredictor();
}

} // namespace lingxi
