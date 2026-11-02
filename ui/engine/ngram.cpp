#include "predictor.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lingxi {

namespace {

constexpr size_t kMaxCands = 9;

bool IsWordChar(wchar_t ch)
{
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'0' && ch <= L'9') || ch == L'_';
}

bool IsCjkChar(wchar_t ch)
{
    return (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF);
}

bool AllCjk(const std::wstring& s)
{
    if (s.empty()) return false;
    for (wchar_t ch : s)
        if (!IsCjkChar(ch)) return false;
    return true;
}

// 词条表：构造时整体排序，Predict 阶段线性扫前缀（1~10 万条 < 0.1ms）
struct WordEntry
{
    std::wstring word;
    uint32_t weight; // en: 100000 - rank；zh: 词频
};

class WordEngine : public IPredictor
{
public:
    WordEngine()
    {
        LoadEn();
        LoadZh();
    }

    Prediction Predict(const std::wstring& ctx) override
    {
        Prediction pred;
        PredictEnglish(ctx, pred);
        if (pred.cands.empty())
            PredictChinese(ctx, pred);
        return pred;
    }

private:
    static std::wstring DataPath(const wchar_t* file)
    {
        wchar_t exePath[MAX_PATH] = L"";
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) return L"";
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (!slash) return L"";
        *slash = L'\0';
        return std::wstring(exePath) + L"\\data\\" + file;
    }

    void LoadEn()
    {
        std::ifstream f(DataPath(L"en_words.txt"));
        if (!f) return;
        std::string line;
        int rank = 0;
        while (std::getline(f, line) && rank < 100000)
        {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string utf8 = line.substr(0, tab);
            if (utf8.empty()) continue;
            int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
            std::wstring w(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wlen);
            if (w.size() < 1 || w.size() > 24) continue;
            _en.push_back({ w, 1000000u - (uint32_t)rank });
            rank++;
        }
    }

    void LoadZh()
    {
        std::ifstream f(DataPath(L"zh_words.txt"));
        if (!f) return;
        std::string line;
        while (std::getline(f, line))
        {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string utf8 = line.substr(0, tab);
            uint32_t freq = (uint32_t)atoi(line.c_str() + tab + 1);
            if (utf8.empty() || freq == 0) continue;
            int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
            std::wstring w(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wlen);
            if (!AllCjk(w)) continue;
            _zh.push_back({ w, freq });
        }
    }

    void PredictEnglish(const std::wstring& ctx, Prediction& pred)
    {
        size_t i = ctx.size();
        while (i > 0 && IsWordChar(ctx[i - 1])) i--;
        std::wstring prefix = ctx.substr(i);
        if (prefix.size() < 2 || _en.empty()) return;

        // 前缀匹配，按词频权重排序取前 5
        std::vector<const WordEntry*> hits;
        for (const auto& e : _en)
        {
            if (e.word.size() > prefix.size() &&
                _wcsnicmp(e.word.c_str(), prefix.c_str(), prefix.size()) == 0)
            {
                hits.push_back(&e);
                if (hits.size() >= 64) break; // 候选池上限，后续排序
            }
        }
        if (hits.empty()) return;
        std::stable_sort(hits.begin(), hits.end(),
                         [](const WordEntry* a, const WordEntry* b) { return a->weight > b->weight; });

        bool capitalize = (prefix[0] >= L'A' && prefix[0] <= L'Z');
        for (size_t k = 0; k < hits.size() && pred.cands.size() < kMaxCands; k++)
        {
            std::wstring word = hits[k]->word;
            if (capitalize && word[0] >= L'a' && word[0] <= L'z')
                word[0] = (wchar_t)towupper(word[0]);
            // 跳过与已输入完全相同的词
            if (_wcsicmp(word.c_str(), prefix.c_str()) == 0) continue;
            pred.cands.push_back(word);
        }
        if (!pred.cands.empty())
            pred.replaceLen = (uint32_t)prefix.size();
    }

    void PredictChinese(const std::wstring& ctx, Prediction& pred)
    {
        // M3 词频表头部占位于 2026-10-29 退役（用户投诉「候选污染」）：
        // 旧版对任何中文上下文返回词频表头部（"了/是/在/和/有/他/不/我/的…"
        // 高频字词），注释自认"质量占位，M4 由 LLM 接管"——LLM 一空
        // （回显剥净/生成失败）整张垃圾列表就漏给用户。中文续写由 LLM
        // 独占；词表引擎只做英文前缀补全。真实中文 ngram 共现模型接入
        // （R3 尾巴）前不再给中文上下文出占位候选。
        (void)ctx;
        (void)pred;
    }

    std::vector<WordEntry> _en;
    std::vector<WordEntry> _zh;
};

} // namespace

IPredictor* CreateDefaultPredictor()
{
    return new WordEngine();
}

// ---- B4：续写回显裁剪 + 上下文感知降级链 ----
// （从 llm.cpp 移出：engine_test 直接编译 ngram.cpp，这两块纯逻辑可单测；
//   llm.cpp 只保留链的装配与 LLM 生成。）

namespace {

// 归一化比较的"可跳过字符"：空白 + 中英常用标点
bool IsEchoSkippable(wchar_t ch)
{
    switch (ch)
    {
    case L' ': case L'\r': case L'\n': case L'\t':
    case L',': case L'.': case L'!': case L'?': case L';': case L':':
    case L'"': case L'\'':
    case L'，': case L'。': case L'！': case L'？': case L'；': case L'：':
    case L'、': case L'…': case L'—': case L'·':
    case 0x201C: case 0x201D: case 0x2018: case 0x2019: // “ ” ‘ ’
        return true;
    default:
        return false;
    }
}

bool IsPureAscii(const std::wstring& s)
{
    for (wchar_t ch : s)
        if ((unsigned short)ch >= 0x80) return false;
    return true;
}

// 候选过滤为纯 ASCII（B4：qwen 续写英文上下文会吐中文候选）。
// cover 与 cands 平行时同步剔除，保持 size()==cands.size() 才有效的约定。
// 返回过滤后是否仍有候选。
bool FilterAsciiCands(Prediction& pred)
{
    if (pred.cands.empty()) return false;
    std::vector<std::wstring> cands;
    std::vector<uint32_t> cover;
    bool parallel = pred.cover.size() == pred.cands.size();
    cands.reserve(pred.cands.size());
    for (size_t k = 0; k < pred.cands.size(); k++)
    {
        if (!IsPureAscii(pred.cands[k])) continue;
        cands.push_back(pred.cands[k]);
        if (parallel) cover.push_back(pred.cover[k]);
    }
    pred.cands = std::move(cands);
    pred.cover = std::move(cover);
    return !pred.cands.empty();
}

// 上下文感知降级链：词中 → 词表补全；纯 ASCII 上下文 → 词表优先 +
// 兜底候选过滤为纯 ASCII（B4）；其余按链序（LLM 优先）。
class ChainPredictor : public IPredictor
{
public:
    ChainPredictor(std::vector<IPredictor*>&& chain) : _chain(std::move(chain)) {}
    ~ChainPredictor()
    {
        for (auto* p : _chain) delete p;
    }

    Prediction Predict(const std::wstring& context) override
    {
        size_t i = context.size();
        while (i > 0 && IsWordChar(context[i - 1])) i--;
        bool midWord = (context.size() - i) >= 2;
        bool pureAscii = !context.empty() && IsPureAscii(context);

        if (_chain.size() > 1 && (midWord || pureAscii))
        {
            // _chain[0]=LLM，_chain[1]=词表
            Prediction pred = _chain[1]->Predict(context);
            if (!pred.cands.empty()) return pred;
            if (pureAscii)
            {
                // B4：词表无果时其余引擎兜底，但候选必须纯 ASCII；
                // 全滤空 → 无候选（英文打字出中文候选比无候选更糟）
                for (auto* p : _chain)
                {
                    Prediction q = p->Predict(context);
                    if (FilterAsciiCands(q)) return q;
                }
                return {};
            }
            // 词中但含 CJK/全角（如"你好ab"）且词表无果：落入链序兜底
        }
        for (auto* p : _chain)
        {
            Prediction pred = p->Predict(context);
            if (!pred.cands.empty()) return pred;
        }
        return {};
    }

private:
    std::vector<IPredictor*> _chain;
};

} // namespace

std::wstring StripContinuationEcho(std::wstring cont, const std::wstring& tail)
{
    // 归一 tail：剥掉尾部空白/标点（回显常与上文尾部只差一个标点形态）
    std::wstring norm = tail;
    while (!norm.empty() && IsEchoSkippable(norm.back())) norm.pop_back();
    if (norm.empty()) return cont;

    std::wstring body = cont;
    bool any = false;
    // 循环剥：输出 = 整段复读 tail 重复 N 遍的形态要剥干净。
    // 只在上轮命中为整段全匹配（k == norm.size()）时续剥：部分重叠
    // （如"很好很好"接"干得很好"）一次即止，避免误伤真实叠词语义。
    for (int round = 0; round < 8 && !body.empty(); round++)
    {
        // 归一比较起点：跳过前导空白/标点（"，天气"式回显）
        size_t pb = 0;
        while (pb < body.size() && IsEchoSkippable(body[pb])) pb++;
        if (pb == body.size())
        {
            // 整段都是空白/标点：首轮保持原样（无回显证据），其后清空
            if (!any) return cont;
            body.clear();
            break;
        }
        size_t avail = body.size() - pb;
        size_t maxK = avail < norm.size() ? avail : norm.size();
        bool matched = false;
        bool full = false;
        // k≥2 阈值：单字重叠歧义太大（叠词续写"很好|很好"会被误切）
        for (size_t k = maxK; k >= 2; k--)
        {
            if (body.compare(pb, k, norm, norm.size() - k) == 0)
            {
                body.erase(0, pb + k);
                // 回显后常紧跟标点/空白起头真续写，去干净
                size_t h2 = 0;
                while (h2 < body.size() && IsEchoSkippable(body[h2])) h2++;
                body.erase(0, h2);
                matched = true;
                full = (k == norm.size());
                any = true;
                break;
            }
        }
        if (!matched || !full) break;
    }
    return any ? body : cont;
}

// ---- 补全候选三重去重（2026-10-29 用户投诉「AI 补全词大量重复」修复）----
// 形态取证见 --probe-typing 与 predictor.h 注释。
namespace {

// 归一（去重比较口径）：剥首尾空白/句读（IsEchoSkippable 同集）
std::wstring NormCandForDedup(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && IsEchoSkippable(s[b])) b++;
    while (e > b && IsEchoSkippable(s[e - 1])) e--;
    return s.substr(b, e - b);
}

// 候选与上文的字符二元组重叠率（标点/空白断组；连续真续写偶有一组撞上
// 不会过阈值，整段复读必过）
double CandCtxOverlap(const std::wstring& cand, const std::wstring& ctx)
{
    size_t total = 0, hit = 0;
    wchar_t prev = 0;
    for (wchar_t ch : cand)
    {
        if (IsEchoSkippable(ch)) { prev = 0; continue; }
        if (prev != 0)
        {
            total++;
            for (size_t k = 1; k < ctx.size(); k++)
                if (ctx[k - 1] == prev && ctx[k] == ch) { hit++; break; }
        }
        prev = ch;
    }
    return total == 0 ? 0.0 : (double)hit / (double)total;
}

} // namespace

std::vector<std::wstring> DedupCompletions(const std::vector<std::wstring>& cands,
                                           const std::wstring& context)
{
    std::vector<std::wstring> out;
    out.reserve(cands.size());
    for (const auto& raw : cands)
    {
        std::wstring n = NormCandForDedup(raw);
        if (n.empty()) continue;
        // ③ 上文复读：二元组重叠率 ≥60% 剔除（0.5B 模型循环复读已输入内容）
        if (n.size() >= 2 && !context.empty() && CandCtxOverlap(n, context) >= 0.6)
            continue;
        bool dropped = false;
        for (size_t k = 0; k < out.size(); k++)
        {
            std::wstring kn = NormCandForDedup(out[k]);
            if (kn == n) { dropped = true; break; }                 // ① 归一相等
            if (kn.size() >= 2 && n.compare(0, kn.size(), kn) == 0) // ② 新项是已选项的延伸
            { dropped = true; break; }                              //   → 保留较短
            if (n.size() >= 2 && kn.compare(0, n.size(), n) == 0)   // ② 已选项是新项的延伸
            { out[k] = raw; dropped = true; break; }                //   → 换成较短
        }
        if (!dropped) out.push_back(raw);
    }
    return out;
}

// 选词联想阶梯呈现（GAP §11 多段呈现，2026-11-02）：联想帧（CTX_F_ASSOC）的
// LLM 长续写拆两级——①首段=首个句读前（含该句读，词级颗粒，如"晴，"）；
// ②全句原样。空格=首段逐段连发（DLL 点选后自动发下一轮联想帧），整句一步选。
// 只拆候选 0（贪心主路）；与 DedupCompletions 规则 ②（前缀对保较短）的关系：
// 那是续写帧防同源堆叠的引擎侧闸，本函数在去重之后由联想帧专属调用——
// 有意的呈现层例外，续写帧零改动。
void StageAssocCands(Prediction& pred)
{
    if (pred.cands.empty()) return;
    const std::wstring& full = pred.cands.front();
    // 首个句读（中英统一，与 SplitContinuation 同集）；句读后须还有字符
    const wchar_t* kStops = L"\u3002\uff0c\uff01\uff1f\u3001\uff1b\uff1a,.!?;:";
    size_t cut = std::wstring::npos;
    for (size_t k = 0; k + 1 < full.size(); k++)
        if (wcschr(kStops, full[k])) { cut = k; break; }
    if (cut == std::wstring::npos) return;
    const size_t segLen = cut + 1; // 首段长度（含句读）
    if (segLen < 2 || segLen > 8) return;      // 词级颗粒：单句读/超长前段不拆
    if (full.size() - segLen < 2) return;      // 剩余太短无阶梯价值
    std::wstring seg = full.substr(0, segLen);
    for (const auto& c : pred.cands)
        if (c == seg) return;                  // 首段已在列（多路采样）不重复插
    pred.cands.insert(pred.cands.begin(), seg);
    // cover/anno 仅在原本平行时同步插入，保持平行不变量
    if (pred.cover.size() == pred.cands.size() - 1) pred.cover.insert(pred.cover.begin(), 0);
    if (pred.anno.size() == pred.cands.size() - 1) pred.anno.insert(pred.anno.begin(), std::wstring());
}

IPredictor* CreateChainPredictor(std::vector<IPredictor*>&& chain)
{
    return new ChainPredictor(std::move(chain));
}

} // namespace lingxi
