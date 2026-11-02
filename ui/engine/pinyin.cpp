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
#include "calcnum.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
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
    case 't': return (initial == "n" || initial == "l") ? "ve" : "ue";
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
    // 输入卫生：小写化并只保留 a-z（与 NormKey 同口径）。大写对（JV）会让
    // 韵母键查表落空、撇号/空格参与两两配对会造成音节错位（ni hao→酿奥）。
    std::string in;
    in.reserve(sp.size());
    for (char ch : sp)
    {
        char c = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        if (c >= 'a' && c <= 'z') in.push_back(c);
    }

    std::string out;
    size_t i = 0;
    for (; i + 1 < in.size(); i += 2)
    {
        char c1 = in[i], c2 = in[i + 1];
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
    if (i < in.size()) out += SpInitial(in[i]); // 残段
    return out;
}

// ---------------------------------------------------------------------------
// R4 拼音纠错（引擎音节切分层，GAP-VS-MSFT §8 R4）：仅当子串不是合法音节时
// 尝试常见误拼改写，改写后仍须命中音节表才生效（方向单一，不做双向猜测）：
//   ① 韵母全写 → 缩写：iou→iu / uei→ui / uen→un（liou→liu、shuei→shui、
//      chuen→chun）——词典与音节表均用缩写形，全写形态今天必落空候选；
//   ② 手滑颠倒：gn → ng（shegn→sheng、magn→mang）；
//   ③ 丢 g 补全：末尾 n → ng（zhon→zhong、din→ding）——仅当补后命中。
// 安全性：合法音节（ren/nv/lve…）永远不进本函数；跨音节边界串（yingnuo）
// 的改写形（yinng/yinnu）不命中音节表自然出局——纠错只救活"打错必挂"的
// 输入，不劫持任何本来合法的切分；与合法路径并存时靠 DP cost 落败让位。
// ---------------------------------------------------------------------------
bool CorrectSyllableTypo(const std::string& s, std::string& out)
{
    if (s.size() < 3) return false;
    static const char* kFull[] = { "iou", "uei", "uen" };
    static const char* kAbbr[] = { "iu", "ui", "un" };
    for (int i = 0; i < 3; i++)
    {
        size_t p = s.find(kFull[i]);
        if (p != std::string::npos)
        {
            out = s.substr(0, p) + kAbbr[i] + s.substr(p + 3);
            return true;
        }
    }
    size_t p = s.find("gn");
    if (p != std::string::npos)
    {
        out = s.substr(0, p) + "ng" + s.substr(p + 2);
        return true;
    }
    if (s.back() == 'n')
    {
        out = s + "g";
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// R3 日期时间直出（GAP-VS-MSFT §8 R3）：整串触发 rq/sj/xq/rqsj（NormalizeIn
// 口径，双拼转换前计算——双拼键序 rq/sj 本无解，触发是纯增益）。候选按
// 本地时间实时生成注入置顶；数字为主，年/月/日/星期 简繁同形，走通用
// 繁化链无害。学习/Boost 均不作用于注入候选（确定性便民，不参与调频）。
// ---------------------------------------------------------------------------

// R7b 大写日期（合同/票据场景）：年逐位转写（2026→二〇二六），月/日按
// 口语读法（十月、三十一日，不补零）。仅 rq 触发注入，不影响常规候选。
std::wstring CnUppercaseDate(const SYSTEMTIME& st)
{
    static const wchar_t* kCnDigit[] = {
        L"〇", L"一", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九"
    };
    wchar_t ys[8];
    _snwprintf_s(ys, _TRUNCATE, L"%u", st.wYear);
    std::wstring y;
    for (const wchar_t* p = ys; *p; ++p)
        y += kCnDigit[*p - L'0'];
    auto cn2 = [](unsigned v) -> std::wstring // 1..31
    {
        if (v < 10) return kCnDigit[v]; // 等长返回，下面两条覆盖 10..31
        if (v == 10) return L"十";
        if (v < 20) return std::wstring(L"十") + kCnDigit[v - 10];
        return std::wstring(kCnDigit[v / 10]) + L"十" +
               ((v % 10) ? std::wstring(kCnDigit[v % 10]) : std::wstring());
    };
    return y + L"年" + cn2(st.wMonth) + L"月" + cn2(st.wDay) + L"日";
}

std::vector<std::wstring> BuildDateTimeCands(const std::string& in)
{
    std::vector<std::wstring> out;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t d[48], t[32], t2[40], b[48];
    static const wchar_t* kDay[] = { L"日", L"一", L"二", L"三", L"四", L"五", L"六" };
    _snwprintf_s(d, _TRUNCATE, L"%u年%02u月%02u日", st.wYear, st.wMonth, st.wDay);
    _snwprintf_s(t, _TRUNCATE, L"%02u:%02u", st.wHour, st.wMinute);
    _snwprintf_s(t2, _TRUNCATE, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    if (in == "rqsj")
    {
        out.push_back(std::wstring(d) + L" " + t);
        out.push_back(std::wstring(d) + L" " + t2);
        _snwprintf_s(b, _TRUNCATE, L"%ls 星期%ls", d, kDay[st.wDayOfWeek]);
        out.push_back(b);
    }
    else if (in == "rq")
    {
        out.push_back(d);
        out.push_back(CnUppercaseDate(st)); // 大写：二〇二六年十月三十一日
        _snwprintf_s(b, _TRUNCATE, L"%u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
        out.push_back(b);
        _snwprintf_s(b, _TRUNCATE, L"%u/%02u/%02u", st.wYear, st.wMonth, st.wDay);
        out.push_back(b);
        _snwprintf_s(b, _TRUNCATE, L"%u年%u月%u日", st.wYear, st.wMonth, st.wDay);
        out.push_back(b); // 无补零：2026年5月3日（单数字月/日场景）
    }
    else if (in == "sj")
    {
        out.push_back(t);
        out.push_back(t2);
        _snwprintf_s(b, _TRUNCATE, L"%u时%u分", st.wHour, st.wMinute);
        out.push_back(b); // 中文时分：14时30分
        unsigned h12 = st.wHour % 12;
        if (h12 == 0) h12 = 12;
        _snwprintf_s(b, _TRUNCATE, L"%ls%u:%02u",
                     st.wHour < 12 ? L"上午" : L"下午", h12, st.wMinute);
        out.push_back(b);
        _snwprintf_s(b, _TRUNCATE, L"%ls%u时%u分",
                     st.wHour < 12 ? L"上午" : L"下午", h12, st.wMinute);
        out.push_back(b); // 12 小时中文：下午2时30分
    }
    else if (in == "xq")
    {
        out.push_back(std::wstring(L"星期") + kDay[st.wDayOfWeek]);
        out.push_back(std::wstring(L"周") + kDay[st.wDayOfWeek]);
        out.push_back(std::wstring(L"礼拜") + kDay[st.wDayOfWeek]);
    }
    return out;
}

// 显式意图候选（R3 日期时间 / R2 自定义短语）置顶注入：注入词去重后置于
// 列表头（注入侧优先，正文中同形候选剔除）；正文保序追后，截断到
// kPredictMaxCandidates。cover 恒 0=整串触发全清语义（提交即整体替换）。
void InjectExplicitTop(Prediction& p, std::vector<uint32_t>& covers,
                       const std::vector<std::wstring>& add)
{
    if (add.empty()) return;
    std::vector<std::wstring> nc;
    std::vector<uint32_t> ncv;
    nc.reserve(add.size() + p.cands.size());
    ncv.reserve(add.size() + p.cands.size());
    std::unordered_set<std::wstring> seen;
    for (const auto& w : add)
        if (!w.empty() && seen.insert(w).second)
        {
            nc.push_back(w);
            ncv.push_back(0);
        }
    for (size_t k = 0; k < p.cands.size() && nc.size() < kPredictMaxCandidates; k++)
        if (seen.insert(p.cands[k]).second)
        {
            nc.push_back(p.cands[k]);
            ncv.push_back(covers[k]);
        }
    if (nc.size() > kPredictMaxCandidates)
    {
        nc.resize(kPredictMaxCandidates);
        ncv.resize(kPredictMaxCandidates);
    }
    p.cands = std::move(nc);
    covers = std::move(ncv);
}

// R5 emoji 候选融合表（静态映射，词→emoji）：候选命中词条时 emoji 紧随其后
// （同 cover——提交时替换等长拼音；只跟随不夺位，触发词必须在候选里）。
// 选词原则：多字稳定词优先，避开"对/错/好"类高频单字霸屏候选窗。
const std::unordered_map<std::wstring, std::wstring>& EmojiFusionMap()
{
    static const std::unordered_map<std::wstring, std::wstring> m = {
        { L"哈哈", L"😄" },   { L"嘿嘿", L"😏" },   { L"微笑", L"🙂" },
        { L"笑哭", L"😂" },   { L"大笑", L"😆" },   { L"大哭", L"😭" },
        { L"眼泪", L"😢" },   { L"爱心", L"❤️" },   { L"心碎", L"💔" },
        { L"玫瑰", L"🌹" },   { L"太阳", L"☀️" },   { L"月亮", L"🌙" },
        { L"星星", L"⭐" },   { L"下雨", L"🌧️" },   { L"下雪", L"❄️" },
        { L"鼓掌", L"👏" },   { L"握手", L"🤝" },   { L"加油", L"💪" },
        { L"点赞", L"👍" },   { L"蛋糕", L"🎂" },   { L"咖啡", L"☕" },
        { L"啤酒", L"🍺" },   { L"火箭", L"🚀" },   { L"恭喜", L"🎉" },
        { L"谢谢", L"🙏" },   { L"生日快乐", L"🎂" }, { L"惊讶", L"😲" },
        { L"生气", L"😡" },   { L"礼物", L"🎁" },   { L"电话", L"📞" },
        { L"邮件", L"📧" },   { L"警告", L"⚠️" },   { L"问号", L"❓" },
        { L"感叹号", L"❗" }, { L"小猫", L"🐱" },   { L"小狗", L"🐶" },
        // R7b 扩充（社交高频）：确认/告别/情绪/生活场景
        { L"收到", L"👌" },   { L"好的", L"👍" },   { L"再见", L"👋" },
        { L"抱歉", L"😞" },   { L"没关系", L"🤗" }, { L"开心", L"😊" },
        { L"难过", L"😔" },   { L"无聊", L"🥱" },   { L"睡觉", L"😴" },
        { L"吃饭", L"🍚" },   { L"庆祝", L"🎊" },   { L"成功", L"✅" },
        { L"失败", L"❌" },   { L"注意", L"❗" },
        // 批次 I 扩充（问候/节日/生活）：Win10 兼容档——Unicode 6.0/E11 为上界，
        // E12+（🧋 等）不入防 Win10 渲染缺字；VS16 对用显式转义防源文件丢不可见字符
        { L"早上好", L"🌞" },   { L"早安", L"🌅" },     { L"晚上好", L"🌃" },
        { L"晚安", L"🌙" },     { L"新年快乐", L"🎆" }, { L"春节", L"🏮" },
        { L"中秋", L"🥮" },     { L"端午", L"🥟" },     { L"圣诞", L"🎄" },
        { L"干杯", L"🍻" },     { L"红包", L"🧧" },     { L"亲亲", L"😘" },
        { L"抱抱", L"🤗" },     { L"可爱", L"🥰" },     { L"我爱你", L"\u2764\uFE0F" },
    };
    return m;
}

// ---------------------------------------------------------------------------
// 音节单元与切分路径
// ---------------------------------------------------------------------------
struct SyllUnit
{
    enum Kind : uint8_t { FULL, INITIAL, PARTIAL };
    Kind kind = FULL;
    std::string text; // 小写
    // 该单元消耗的原始输入字符数。常规单元 text 即原始输入、0=与 text 等长；
    // B3 零声母简写单元的 text 是改写后的规范音节（i→yi 长度≠键数），
    // 必须显式记录原始消耗，cover 回放按此值前进。
    int rawLen = 0;
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
    // 该候选消耗的规范化输入前缀字符数（in 下标；0=未计算）。
    // Predict 再经 inOfCtx 映射回原始 context 字符数填入 Prediction.cover。
    int coverEnd = 0;
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
        // v 模式（数字大写/计算器）：确定性候选，拦截在拼音词典处理前；
        // cover=整串消耗（提交后全清），Learn 侧拒收 IsCalcQuery 键。
        if (IsCalcQuery(context))
        {
            p.cands = CalcCandidates(context);
            p.cover.assign(p.cands.size(), (uint32_t)context.size());
            p.replaceLen = 0;
            return p;
        }
        Settings st = LoadSettings();
        _fuzzy = st.fuzzyPinyin; // 开关实时生效
        if (st.fuzzyGroups >= 1 && st.fuzzyGroups <= 0xFF && st.fuzzyGroups != _fuzzyGroups)
        {
            // 组掩码变更：重建预计算模糊索引（音节量级小，毫秒级，仅变更时发生）
            _fuzzyGroups = st.fuzzyGroups;
            BuildFuzzyIndex();
        }
        std::string raw = ToUtf8(context);
        if (raw.empty()) return p;
        // B1 部分提交 cover：仅全 ASCII 且非双拼时提供。双拼键位→全拼映射
        // 不可逆推（cover 会指错位置）；非 ASCII 字符 UTF-8 变长会让 in 下标
        // 与 context 错位。两种情况一律不提供（消费方按 0 处理=旧行为）。
        bool coverOk = !st.shuangpin && raw.size() == context.size();
        std::string inForCover;
        std::vector<uint32_t> inOfCtx; // in 下标 → 原始 context 下标（撇号计入）
        if (coverOk)
        {
            inForCover = NormalizeIn(raw);
            inOfCtx.reserve(inForCover.size());
            for (size_t i = 0; i < raw.size(); i++)
            {
                char c = Lower(raw[i]);
                // coverOk 保证 raw 与 context 同长（纯 ASCII 单字节），
                // 只需匹配字母与直撇号；弯撇号（UTF-8 多字节）走不进这里。
                if ((c >= 'a' && c <= 'z') || raw[i] == '\'')
                    inOfCtx.push_back((uint32_t)i);
            }
            if (inOfCtx.size() != inForCover.size()) coverOk = false;
        }
        // R3/R2 整串触发输入（NormalizeIn 口径）：必须在双拼转换前计算——
        // 转换后字母语义已变（双拼 rq/sj 键序本无解，触发是纯增益）。
        // 触发串为纯字母，EnumeratePaths 的单字母简拼兜底保证恒有路径，
        // 不会落进下面 paths.empty() 早退分支。
        std::string inTrig = NormalizeIn(raw);
        std::vector<std::wstring> dtCands = BuildDateTimeCands(inTrig);
        std::vector<std::wstring> phraseCands; // R2：锁内拷出（见下），锁外注入
        if (st.shuangpin) raw = ShuangpinToQuanpin(raw); // 双拼→全拼
        // 显式隔音判定（与 EnumeratePaths 同一归一口径，弯撇号 U+2019 同算）：
        // 有隔音时 MatchWords 紧校验音节数（拒跨边界 compact 撞键），无隔音时
        // 串级匹配是合法读法不校验（xian 的"西安"走 [xian] 路径 tier-0）。
        // 必须在双拼转换后计算：ShuangpinToQuanpin 滤撇号，双拼自然无隔音。
        bool strictSep = NormalizeIn(raw).find('\'') != std::string::npos;

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

        std::vector<Candidate> out = Generate(paths, coverOk ? &inForCover : nullptr, strictSep);
        for (const auto& c : out)
            LX_LOGV(L"py", L"cand cost=%.2f %hs", c.cost, ToUtf8(c.word).c_str());

        // R6 屏蔽词：锁内一次拷出（通常空集，微秒级），去重循环与 ups 注入
        // 同口径过滤；显式意图（dt/短语/emoji）不受影响——用户显式配置优先。
        std::unordered_set<std::wstring> blocked;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            if (!_blocked.empty()) blocked = _blocked;
        }

        // 去重（保序，已按优先级排序）；cover 经 inOfCtx 映射回 context 字符数
        std::unordered_set<std::wstring> seen;
        std::vector<uint32_t> covers;
        covers.reserve(out.size());
        for (auto& c : out)
        {
            if (c.word.empty() || seen.count(c.word)) continue;
            if (!blocked.empty() && blocked.count(c.word)) continue;
            seen.insert(c.word);
            uint32_t cv = 0;
            if (coverOk && c.coverEnd > 0 && (size_t)c.coverEnd <= inOfCtx.size())
                cv = inOfCtx[(size_t)c.coverEnd - 1] + 1;
            p.cands.push_back(c.word);
            covers.push_back(cv);
            if (p.cands.size() >= kPredictMaxCandidates) break;
        }

        // R5 emoji 候选融合：命中映射表的词后紧跟 emoji（同 cover——提交时
        // 替换等长拼音）。只追加跟随不夺位：触发词必须在候选中且永排第一，
        // emoji 永远第二；同 emoji 多触发词只融合一次（seen 去重）。
        {
            const auto& em = EmojiFusionMap();
            std::vector<std::wstring> fused;
            std::vector<uint32_t> fusedCovers;
            bool fusedAny = false;
            for (size_t k = 0; k < p.cands.size(); k++)
            {
                fused.push_back(p.cands[k]);
                fusedCovers.push_back(covers[k]);
                auto eit = em.find(p.cands[k]);
                if (eit != em.end() && seen.insert(eit->second).second)
                {
                    fused.push_back(eit->second);
                    fusedCovers.push_back(covers[k]);
                    fusedAny = true;
                }
            }
            if (fusedAny)
            {
                if (fused.size() > kPredictMaxCandidates)
                {
                    fused.resize(kPredictMaxCandidates);
                    fusedCovers.resize(kPredictMaxCandidates);
                }
                p.cands = std::move(fused);
                covers = std::move(fusedCovers);
            }
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
            // （键规范化与 _userDict 同口径，撇号/空格不参与查找）
            if (key.size() >= 2) // 单字符缩写召回=用户历史长句霸屏（"过拟合"投诉），禁止
            if (auto iit = _userByInit.find(NormKey(raw)); iit != _userByInit.end())
            {
                for (const auto& ip : iit->second)
                {
                    bool dup = false;
                    for (const auto& u : ups)
                        if (u.word == ip.word) { dup = true; break; }
                    if (!dup) ups.push_back({ ip.word, ip.count });
                }
            }
            // R2 自定义短语：整串精确命中缩写（同锁拷出，注入在锁外）
            if (auto pit = _phrases.find(inTrig); pit != _phrases.end())
                phraseCands = pit->second;
        }
        // R6：自学习候选同口径过滤（屏蔽词对用户历史同样生效）
        if (!blocked.empty() && !ups.empty())
            ups.erase(std::remove_if(ups.begin(), ups.end(),
                                     [&](const UserPick& u) {
                                         return blocked.count(u.word) > 0;
                                     }),
                      ups.end());

        // R2/R3 显式意图注入：整串触发的确定性候选置顶——先注入，再走自学习
        // 置顶重排（习惯优先于便民：用户在该串下反复选过的词仍排最前）。
        // 注入序=先日期时间后短语（InjectExplicitTop 前插）→ 短语 > 日期时间 > 常规。
        if (!dtCands.empty())
            InjectExplicitTop(p, covers, dtCands);
        if (!phraseCands.empty())
            InjectExplicitTop(p, covers, phraseCands);

        if (!ups.empty())
        {
            std::stable_sort(ups.begin(), ups.end(),
                             [](const UserPick& a, const UserPick& b) {
                                 if (a.count != b.count) return a.count > b.count;
                                 return a.word < b.word;
                             });
            std::vector<std::wstring> reordered;
            std::vector<uint32_t> reorderedCover;
            reordered.reserve(p.cands.size() + ups.size());
            reorderedCover.reserve(p.cands.size() + ups.size());
            std::unordered_set<std::wstring> promoted;
            for (const auto& up : ups)
                if (promoted.insert(up.word).second)
                {
                    reordered.push_back(up.word);
                    reorderedCover.push_back(0); // 用户词典 boost：cover 未知，保守全清
                }
            for (size_t k = 0; k < p.cands.size(); k++)
                if (promoted.insert(p.cands[k]).second)
                {
                    reordered.push_back(p.cands[k]);
                    reorderedCover.push_back(covers[k]);
                }
            if (reordered.size() > kPredictMaxCandidates)
            {
                reordered.resize(kPredictMaxCandidates);
                reorderedCover.resize(kPredictMaxCandidates);
            }
            p.cands = std::move(reordered);
            covers = std::move(reorderedCover);
        }

        if (st.traditionalOutput)
            for (auto& w : p.cands) w = ToTraditional(w);

        p.replaceLen = 0;
        p.cover = std::move(covers);
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
    unsigned _fuzzyGroups = 0x1F; // 模糊音组位掩码，见 Settings.fuzzyGroups
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
    // 后台落盘互斥位：磁盘 I/O 移出 _udMtx（持锁写盘+全量重建曾让 Predict
    // 饥饿数百 ms——实测选字后下一帧候选响应 464ms，提交键超时上屏字母）。
    std::atomic<bool> _persistBusy{false};
    // R2 自定义短语（GAP §8 R2）：缩写（NormalizeIn 口径小写字母串）→ 短语
    // 列表（同缩写多条按文件顺序全注入）。phrases.txt 与 userdict 同目录同锁
    // （_udMtx）——LoadUserDict 一并重载，UI 保存后经 ReloadUserDict 热生效。
    std::unordered_map<std::string, std::vector<std::wstring>> _phrases;
    // R6 屏蔽词（GAP §8 R6 长尾池）：blocked.txt 每行一个词，Generate 输出
    // 与自学习候选同口径过滤（显式意图 dt/短语/emoji 不受影响——用户显式
    // 配置优先）。同 _udMtx 锁，LoadUserDict 通道热载。
    std::unordered_set<std::wstring> _blocked;
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

    // 规范化：仅保留 a-z（小写）；撇号（ASCII/弯体）作为切分边界保留。
    // 预测与 cover 映射共用同一口径（同字符同序，仅过滤）。
    // 弯撇号（U+2019/U+2018）是 UTF-8 三字节序列（E2 80 99/98）——窄字面量
    // 与单字节 char 比较恒假（旧 `ch=='\u2019'` 是死代码），必须按序列探测；
    // 归一成同一个隔音边界后弯/直撇号行为一致（cover 映射仍受"UTF-8 变长
    // 不映射"限制，弯体输入 cover=0，与 Predict 口径一致）。
    static std::string NormalizeIn(const std::string& rawIn)
    {
        std::string in;
        in.reserve(rawIn.size());
        for (size_t i = 0; i < rawIn.size(); i++)
        {
            char ch = rawIn[i];
            char c = Lower(ch);
            if (c >= 'a' && c <= 'z') in.push_back(c);
            else if (ch == '\'') in.push_back('\'');
            else if (i + 2 < rawIn.size() &&
                     (unsigned char)rawIn[i] == 0xE2 && (unsigned char)rawIn[i + 1] == 0x80 &&
                     ((unsigned char)rawIn[i + 2] == 0x99 || (unsigned char)rawIn[i + 2] == 0x98))
            {
                in.push_back('\'');
                i += 2; // 跳过 UTF-8 剩余两字节
            }
        }
        return in;
    }

    // 零声母简写（B3）：人类输入习惯里 i/u 打头的韵母常省略 y/w
// （i→yi、in→yin、iu→you、u→wu、ui→wei、uo→wo…）。词典与音节表均无
// i/u 打头的写法，不补这层时首键 i/u 起的组合永远空候选——即用户报的
// "输入 i/u 特殊拼音后按键像失灵"根因（数字被吞、候选空）。仅在切分
// 起点查表，不改 mid-string 拼写；v 不在表内（v 非拼音——DLL 侧 v 走拼音
// 组合，引擎 0 候选，空格/回车 CommitRaw 原样上屏 "v"/"vlog"，即 v 临时英文）。
static const char* ZeroInitialShorthand(const std::string& s)
{
    static const std::pair<const char*, const char*> kMap[] = {
        { "i", "yi" },      { "ia", "ya" },      { "iao", "yao" },
        { "ian", "yan" },   { "iang", "yang" },  { "ie", "ye" },
        { "in", "yin" },    { "ing", "ying" },   { "iong", "yong" },
        { "iu", "you" },
        { "u", "wu" },      { "ua", "wa" },      { "uai", "wai" },
        { "uan", "wan" },   { "uang", "wang" },  { "ue", "yue" },
        { "ueng", "weng" }, { "ui", "wei" },     { "un", "wen" },
        { "uo", "wo" },
    };
    for (const auto& kv : kMap)
        if (s == kv.first) return kv.second;
    return nullptr;
}

// 拟声音节判定：不含 a/e/i/o/u 元音的"音节"（m/n/hm→呣/嗯/噷 等叹词注音）。
// 它们在音节表里合法（单独输入仍需出字），但作为 FULL 单元参与 DP 时若
// 成本低于简拼(4/格)，[z][m:FULL 2]=6 会把正常简拼链 [z][m]=8 压到次级
// tier，表现为 zm→"在呣"、zn→"在嗯" 这类拼装垃圾盖过"怎么/字母"。
// 故对这类音节加罚 5（>4）：正常简拼路径永远 tier 优先，拟声只兜底。
static bool IsInterjectionSyllable(const std::string& s)
{
    for (char c : s)
        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')
            return false;
    return !s.empty();
}

std::vector<SegPath> EnumeratePaths(const std::string& rawIn)
    {
        std::string in = NormalizeIn(rawIn);
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
                if (_syllables.count(s))
                {
                    // 无元音拟声音节（m/n/hm）加罚 5：高于简拼(4)，不再
                    // 压制 [z][m]→"怎么/字母" 的正常简拼链；与简拼并存
                    // 于不同 tier，单独打 m/n 时呣/嗯 仍可作为候选出现。
                    int cost = IsInterjectionSyllable(s) ? 5 : 2;
                    Extend(dp, i, i + L, SyllUnit{ SyllUnit::FULL, s }, cost);
                    continue;
                }
                // R4 拼音纠错：非合法音节时尝试常见误拼改写，改写命中音节表
                // 才扩展（rawLen=原串长，供 cover 回放不错位）。加 3 罚分：
                // 高于简拼(4/格)的正常路径——正常读法永远优先，仅救活
                // "打错必挂"的输入；残段(PARTIAL)纠错暂不支持。
                std::string fixedSyl;
                if (CorrectSyllableTypo(s, fixedSyl) && _syllables.count(fixedSyl))
                    Extend(dp, i, i + L, SyllUnit{ SyllUnit::FULL, fixedSyl, L }, 5);
            }
            // 零声母简写兜底（B3）：i/u 打头省略 y/w 的写法作为完整音节
            // 单元扩展，与上方逐字全拼/简拼并存，由 DP cost 自然竞争；
            // rawLen 记录原始键数（text 是改写后的规范音节）。
            for (int L = 1; L <= 4 && i + L <= bound; L++)
            {
                const char* t = ZeroInitialShorthand(in.substr(i, L));
                if (t)
                    Extend(dp, i, i + L, SyllUnit{ SyllUnit::FULL, t, L }, 2);
            }
            // 单字母简拼。v 例外（#1 v 重设计）：v 非声母（词典无 v 起音节，
            // nv/lv 走全音节表），若作简拼 INITIAL 会造出 [v][k-PARTIAL] 这类
            // 死头路径——v 单元永无词可配，但残段池照常发射 k 声母字（vk→可/
            // 会/看，vlog→个/过/给），空格选字上屏，CommitRaw 永不触发，
            // "vlog 语义"落空。排除后 v 起串无任何路径→0 候选→回显+空格
            // CommitRaw 原样上屏，即设计意图（见 ZeroInitialShorthand 注释）。
            if (in[i] >= 'a' && in[i] <= 'z' && in[i] != 'v')
                Extend(dp, i, i + 1, SyllUnit{ SyllUnit::INITIAL, std::string(1, in[i]) }, 4);
        }

        // 收集完整路径 + 末尾残段路径——分池保留，再合并截断。
        // 旧实现把两池塞进同一个 Prune 全局留 K_POS=7，随后的 K_FINAL=14
        // 截断形同虚设（死代码）；长串输入时低 cost 的完整路径吃光配额，
        // 末尾残段（"…des" 这种正在打的最后一个音节）被整池挤掉，
        // 表现为：拼音越长按空格越容易上屏字母原文。
        // 现在：完整/残段各留 top K_POS（beam 内已各自按 cost 收敛），
        // stable_sort 合并去重后限 K_FINAL——两池都保得住，路径数上限 14。
        std::vector<SegPath> fullPool, partPool;
        if (!dp[n].empty())
            for (const auto& p : dp[n])
                fullPool.push_back(p);

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
                partPool.push_back(std::move(np));
            }
        }

        Prune(fullPool, K_POS);
        Prune(partPool, K_POS);

        std::vector<SegPath> all = std::move(fullPool);
        all.insert(all.end(), std::make_move_iterator(partPool.begin()),
                   std::make_move_iterator(partPool.end()));
        Prune(all, K_FINAL);
        return all;
    }

    // 从 i 位置扩展切分路径：对 dp[from] 里的每条路径追加单元，写入 dp[to]，
    // 随即 Prune（去重 + 稳定按 cost 截断到 K_POS 的 DP 梁宽）。
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

    // R2 自定义短语存储（与 userdict 同目录；UI 侧 phraseui.cpp 同格式写）
    static std::string PhrasesPath()
    {
        char buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::string dir = std::string(buf) + "\\LingxiIme";
            CreateDirectoryA(dir.c_str(), NULL);
            return dir + "\\phrases.txt";
        }
        return ExeDir() + "\\phrases.txt";
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
        if (!raw.empty())
        {
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
        }
        BuildUserInitIndex();
        LX_LOG(L"py", L"用户词典加载: %zu 个拼音键, %zu 个简拼键",
               _userDict.size(), _userByInit.size());
        LoadPhrasesLocked(); // R2：与用户词典同一通道热载（同锁）
        LoadBlockedLocked(); // R6：屏蔽词同通道热载（同锁）
    }

    // R2 短语装载（调用方持 _udMtx）：行格式 缩写<TAB>短语（UTF-8，BOM 容忍）。
    // 缩写归一=只留小写 a-z（2..16 位）且必须纯字母；短语非空 ≤200 字符。
    void LoadPhrasesLocked()
    {
        _phrases.clear();
        std::string raw = ReadAll(PhrasesPath());
        if (raw.empty()) return;
        for (auto& line : Split(raw, '\n'))
        {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.size() >= 3 && (unsigned char)l[0] == 0xEF &&
                (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF)
                l.erase(0, 3); // UTF-8 BOM
            size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            std::string abbrev = l.substr(0, tab);
            std::wstring phrase = FromUtf8(l.substr(tab + 1));
            if (abbrev.empty() || phrase.empty()) continue;
            std::string norm;
            for (char c : abbrev)
            {
                char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                if (lc >= 'a' && lc <= 'z') norm.push_back(lc);
            }
            if (norm.size() < 2 || norm.size() > 16 || norm.size() != abbrev.size() ||
                phrase.size() > 200)
                continue;
            _phrases[norm].push_back(std::move(phrase));
        }
        LX_LOG(L"py", L"自定义短语加载: %zu 个缩写", _phrases.size());
    }

    // R6 屏蔽词存储（与 userdict/phrases 同目录；每行一个词，UTF-8 BOM 容忍）
    static std::string BlockedPath()
    {
        char buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::string dir = std::string(buf) + "\\LingxiIme";
            CreateDirectoryA(dir.c_str(), NULL);
            return dir + "\\blocked.txt";
        }
        return ExeDir() + "\\blocked.txt";
    }

    // R6 屏蔽词装载（调用方持 _udMtx）：词≤50 字符，上限 500 条
    void LoadBlockedLocked()
    {
        _blocked.clear();
        std::string raw = ReadAll(BlockedPath());
        if (raw.empty()) return;
        for (auto& line : Split(raw, '\n'))
        {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.size() >= 3 && (unsigned char)l[0] == 0xEF &&
                (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF)
                l.erase(0, 3); // UTF-8 BOM
            std::wstring w = FromUtf8(l);
            if (w.empty() || w.size() > 50 || _blocked.size() >= 500) continue;
            _blocked.insert(std::move(w));
        }
        LX_LOG(L"py", L"屏蔽词加载: %zu 条", _blocked.size());
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

    // 单键增量更新简拼索引（调用方持 _udMtx）：Learn 热路径专用，替代旧的
    // 锁内全量重建（全量 = 每键一次 EnumeratePaths，词表大时数百 ms 持锁，
    // Predict 同锁被饿死）。语义与 BuildUserInitIndex 对该键完全一致。
    void UpdateUserInitIndexLocked(const std::string& key)
    {
        // 擦旧：该全拼键的条目可能落在任意首字母桶（淘汰旧词后也须重写桶
        // 内容），索引小，全索引扫一遍即可
        for (auto it = _userByInit.begin(); it != _userByInit.end();)
        {
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&key](const UserInitPick& p) { return p.fullKey == key; }),
                      vec.end());
            if (vec.empty()) it = _userByInit.erase(it);
            else ++it;
        }
        auto dit = _userDict.find(key);
        if (dit == _userDict.end() || dit->second.empty()) return;
        // 与 BuildUserInitIndex 同口径：最优切分（cost 最小）取首字母串
        std::vector<SegPath> sp = EnumeratePaths(key);
        if (sp.empty()) return;
        const SegPath* best = &sp[0];
        for (const auto& p : sp)
            if (p.cost < best->cost) best = &p;
        std::string init;
        for (const auto& u : best->units)
            if (!u.text.empty()) init.push_back(u.text[0]);
        if (init.empty()) return;
        auto& vec = _userByInit[init];
        for (const auto& up : dit->second)
            vec.push_back({ up.word, up.count, key });
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

    // 序列化用户词典（调用方持 _udMtx），文件格式不变
    std::string SerializeUserDictLocked() const
    {
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
        return content;
    }

    // 落盘（调用方必须已持有 _udMtx）。失败保持 _userDirty，下次学习时重试。
    bool SaveUserDictLocked()
    {
        if (!_userDirty) return true;
        if (!WriteFileAtomicA(UserDictPath(), SerializeUserDictLocked())) return false;
        _userDirty = false;
        return true;
    }

    void SaveUserDict()
    {
        std::lock_guard<std::mutex> lk(_udMtx);
        SaveUserDictLocked();
    }

    // 后台落盘：Learn 跑在管道读线程，持 _udMtx 同步写盘会延迟 CTX 帧分发
    // 并让 Predict 同锁饿数百 ms。快照→清脏→锁外原子写；_persistBusy 保证
    // 同一时刻只有一个落盘线程（Learn 有管道读线程与 UI 拾取回调两个入口）。
    void KickPersist()
    {
        bool idle = false;
        if (!_persistBusy.compare_exchange_strong(idle, true))
            return; // 已有线程在跑：它收尾时会重查脏位
        std::thread([this]()
        {
            for (;;)
            {
                std::string content;
                {
                    std::lock_guard<std::mutex> lk(_udMtx);
                    if (!_userDirty) break;
                    content = SerializeUserDictLocked();
                    _userDirty = false; // 快照即清脏：写盘期间的新学习会重新置脏
                }
                if (!WriteFileAtomicA(UserDictPath(), content))
                {
                    std::lock_guard<std::mutex> lk(_udMtx);
                    _userDirty = true; // 写失败恢复脏位，下次重试（原语义）
                    break;
                }
            }
            _persistBusy.store(false);
            // 补漏：置 idle 与新 Learn 的 CAS 失败之间有窗口，重查一次脏位
            bool dirtyNow = false;
            {
                std::lock_guard<std::mutex> lk(_udMtx);
                dirtyNow = _userDirty;
            }
            if (dirtyNow) KickPersist();
        }).detach();
    }

    // 用户选择反馈：该拼音串下，被选词计数 +1，落盘后下次同串置顶。
    void Learn(const std::wstring& rawPinyin, const std::wstring& word) override
    {
        if (rawPinyin.empty() || word.empty()) return;
        if (IsCalcQuery(rawPinyin)) return; // v 模式：确定性结果不进自学习（防词典污染）
        std::string key = ToUtf8(rawPinyin);
        // 双拼模式下 rawPinyin 是双拼串，而 Predict 以转换后的全拼串查键——
        // 不做同一转换，学习键与查询键永不相交，双拼下学习完全失效
        if (LoadSettings().shuangpin) key = ShuangpinToQuanpin(key);
        key = NormKey(key);
        if (key.empty()) return;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            auto& vec = _userDict[key];
            bool found = false;
            for (auto& up : vec)
                if (up.word == word) { up.count++; found = true; break; }
            if (!found)
            {
                if (vec.size() >= 8)
                {
                    // 每键最多 8 个偏好：淘汰计数最小者（保留最常用），
                    // 而非最近添加的
                    size_t victim = 0;
                    for (size_t k = 1; k < vec.size(); k++)
                        if (vec[k].count < vec[victim].count) victim = k;
                    vec.erase(vec.begin() + victim);
                }
                vec.push_back({ word, 1 });
            }
            _userDirty = true;
            UpdateUserInitIndexLocked(key); // 单键增量：新词即时可简拼调出
        }
        KickPersist(); // 磁盘 I/O 后台化：Predict/CTX 分发不再等写盘
        LX_LOG(L"py", L"学习: '%hs' -> '%hs'", key.c_str(), ToUtf8(word).c_str());
    }

    // 管理界面清空/导入后重新加载用户词典，立即生效。先等在途落盘收敛再读
    // 文件：否则后台旧快照回写后重载，会把引擎内存整体回退到旧文件内容。
    void ReloadUserDict() override { FlushUserDict(); LoadUserDict(); }

    // ---- 用户词典管理操作（2026-11-01"删除词条复活"根治）----
    // 旧模型：UI 直接改 userdict.txt + ReloadUserDict，与后台落盘竞态——
    // KickPersist 快照（持锁）→清脏→锁外写，UI 的删除/清空可被在途旧快照
    // 原样写回（词条"复活"，列表刷新即见，用户"说了n遍还没加上"的实感根
    // 源）。新模型：管理操作全部经引擎持 _udMtx 原子改内存+置脏，由唯一写
    // 者（KickPersist 线程）收敛落盘；FlushUserDict 等收敛后返回，UI 再刷
    // 新列表，所见即引擎内存。

    // 等词典落盘收敛（内存→文件同态）：管理操作后、UI 刷新/导出前调用。
    // 上限 ~2s：写失败（盘满/权限）不卡死 UI，保持"脏位重试"原语义。
    void FlushUserDict()
    {
        KickPersist();
        for (int i = 0; i < 2000; i++)
        {
            if (!_persistBusy.load())
            {
                std::lock_guard<std::mutex> lk(_udMtx);
                if (!_userDirty) return;
            }
            Sleep(1);
        }
    }

    bool RemoveUserWord(const std::wstring& word, const std::wstring& key) override
    {
        if (word.empty()) return false;
        // 键双口径匹配：文件键历史来源不一（Learn 写 NormKey 归一键，早期导
        // 入可能带大写/分隔符），精确或归一命中皆认，防"看得见删不掉"。
        std::string kExact = ToUtf8(key);
        std::string kNorm = NormKey(kExact);
        bool filterKey = !kNorm.empty(); // key 空=删该词全部读音
        bool removed = false;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            for (auto it = _userDict.begin(); it != _userDict.end();)
            {
                if (filterKey && it->first != kExact && it->first != kNorm) { ++it; continue; }
                auto& vec = it->second;
                size_t before = vec.size();
                for (size_t i = 0; i < vec.size(); )
                {
                    if (vec[i].word == word) vec.erase(vec.begin() + i);
                    else ++i;
                }
                if (vec.size() != before)
                {
                    removed = true;
                    UpdateUserInitIndexLocked(it->first);
                }
                if (vec.empty()) it = _userDict.erase(it);
                else ++it;
            }
            if (removed) _userDirty = true;
        }
        if (removed)
        {
            FlushUserDict();
            LX_LOG(L"py", L"词典删除: '%ls' key='%ls'", word.c_str(), key.c_str());
        }
        return removed;
    }

    bool ClearUserDict() override
    {
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            if (_userDict.empty()) return true;
            _userDict.clear();
            _userByInit.clear();
            _userDirty = true;
        }
        FlushUserDict();
        LX_LOG(L"py", L"词典清空");
        return true;
    }

    bool AddUserWord(const std::wstring& word, const std::wstring& key, int gain) override
    {
        if (word.empty() || gain <= 0) return false;
        std::string kNorm = NormKey(ToUtf8(key)); // 新增键一律归一（Learn 口径）
        if (kNorm.empty()) return false;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            auto& vec = _userDict[kNorm];
            bool found = false;
            for (auto& up : vec)
                if (up.word == word) { up.count += gain; found = true; break; }
            if (!found) vec.push_back({ word, gain });
            _userDirty = true;
            UpdateUserInitIndexLocked(kNorm); // 新词即时可简拼调出
        }
        FlushUserDict();
        LX_LOG(L"py", L"词典新增: '%ls' key='%hs' +%d", word.c_str(), kNorm.c_str(), gain);
        return true;
    }

    bool NudgeUserWord(const std::wstring& word, const std::wstring& key, int delta) override
    {
        if (word.empty() || delta == 0) return false;
        std::string kExact = ToUtf8(key);
        std::string kNorm = NormKey(kExact);
        bool filterKey = !kNorm.empty(); // key 空=该词全部读音一起调
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            for (auto& kv : _userDict)
            {
                if (filterKey && kv.first != kExact && kv.first != kNorm) continue;
                bool hit = false;
                for (auto& up : kv.second)
                {
                    if (up.word == word)
                    {
                        up.count += delta;
                        if (up.count < 1) up.count = 1;
                        hit = true;
                        found = true;
                    }
                }
                if (hit) UpdateUserInitIndexLocked(kv.first);
            }
            if (found) _userDirty = true;
        }
        if (found) FlushUserDict();
        return found;
    }

    bool MergeUserDict(const std::vector<UserDictEntry>& entries) override
    {
        if (entries.empty()) return false;
        {
            std::lock_guard<std::mutex> lk(_udMtx);
            for (const auto& e : entries)
            {
                std::string kNorm = NormKey(ToUtf8(e.key)); // 导入键归一，查询可命中
                if (e.word.empty() || kNorm.empty() || e.count <= 0) continue;
                auto& vec = _userDict[kNorm];
                bool merged = false;
                for (auto& up : vec)
                    if (up.word == e.word) { up.count += e.count; merged = true; break; }
                if (!merged) vec.push_back({ e.word, e.count });
                UpdateUserInitIndexLocked(kNorm);
            }
            _userDirty = true;
        }
        FlushUserDict();
        LX_LOG(L"py", L"词典导入合并: %zu 条", entries.size());
        return true;
    }

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
        // 组位掩码：bit0=zh/z bit1=ch/c bit2=sh/s bit3=n/l（细分开关，对标微软拼音）
        // 注意独立 if 非 else-if 链：同一声母可挂多组（"l" 同属 bit3 与 bit6），
        // else-if 会令后写分支被先命中分支遮挡（审计轮 5 修复——修复前双组
        // 同开时 l→r 永不可达，r→l 却正常，单向不对称）
        if (initial == "z" && (_fuzzyGroups & 1)) initVar.push_back("zh");
        if (initial == "zh" && (_fuzzyGroups & 1)) initVar.push_back("z");
        if (initial == "c" && (_fuzzyGroups & 2)) initVar.push_back("ch");
        if (initial == "ch" && (_fuzzyGroups & 2)) initVar.push_back("c");
        if (initial == "s" && (_fuzzyGroups & 4)) initVar.push_back("sh");
        if (initial == "sh" && (_fuzzyGroups & 4)) initVar.push_back("s");
        if (initial == "n" && (_fuzzyGroups & 8)) initVar.push_back("l");
        if (initial == "l" && (_fuzzyGroups & 8)) initVar.push_back("n");
        // f/h、l/r、g/k 三组：对标微软拼音更多组细分（GAP §1）
        if (initial == "f" && (_fuzzyGroups & 0x20)) initVar.push_back("h");
        if (initial == "h" && (_fuzzyGroups & 0x20)) initVar.push_back("f");
        if (initial == "r" && (_fuzzyGroups & 0x40)) initVar.push_back("l");
        if (initial == "l" && (_fuzzyGroups & 0x40)) initVar.push_back("r");
        if (initial == "g" && (_fuzzyGroups & 0x80)) initVar.push_back("k");
        if (initial == "k" && (_fuzzyGroups & 0x80)) initVar.push_back("g");

        std::vector<std::string> finVar{ final };
        if ((_fuzzyGroups & 0x10) &&
            final.size() >= 2 && final[final.size() - 1] == 'g' &&
            final[final.size() - 2] == 'n')
        {
            finVar.push_back(final.substr(0, final.size() - 1)); // -ng → -n
        }
        else if ((_fuzzyGroups & 0x10) && !final.empty() && final.back() == 'n')
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

    // 去重（相同单元序列）+ 按 cost 稳定排序 + 截断到 keep。
    // 稳定排序是刻意的：同 cost 的平局（多条路径/多个词频相同的词）按生成序
    // 保留，截断才不抖动——否则同一次输入的两次调用可能给出不同候选池。
    static void Prune(std::vector<SegPath>& v, int keep = K_POS)
    {
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
            if (keys.insert(k).second)
                uniq.push_back(std::move(p));
        }
        std::stable_sort(uniq.begin(), uniq.end(),
                         [](const SegPath& a, const SegPath& b) { return a.cost < b.cost; });
        if ((int)uniq.size() > keep) uniq.resize(keep);
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

    // strictSep：输入含显式隔音撇号（NormalizeIn 口径，弯撇号同）。有隔音时
    // 紧凑/模糊分支紧校验音节数——跨显式边界的串级撞键（xi'an 的 [xi][an]
    // 路径撞单音节词"先"）违反用户标注的边界，拒收；无隔音时串级匹配本来就是
    // 合法读法（xian 的"西安"走 [xian] 单元路径 tier-0），不能拒。
    std::vector<WordMatch> MatchWords(const std::vector<SyllUnit>& units, bool strictSep)
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
                    for (int i : it->second)
                    {
                        // 紧凑全拼丢边界（xi+an 与 xian 同串）：单音节词"先/建"
                        // 会撞进双单元路径——隔音符（xi'an）语义因此失效。
                        // 音节数必须与单元数一致（PARTIAL 分支 1176 同款校验）。
                        // 有隔音时拒收跨显式边界的撞键词（xi'an 不出"先"）；
                        // 无隔音时不校验（xian 的"西安"合法走 [xian] 路径）。
                        if (strictSep && _entries[i].sylls.size() != (size_t)n) continue;
                        add(i); exactEi.insert(i);
                    }
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
                            if (strictSep && _entries[ei].sylls.size() != (size_t)n) continue; // 跨隔音撞键：同上
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
    // inForCover：规范化输入串（与 EnumeratePaths 内部 in 同一来源）；
    // 非空时逐候选计算 coverEnd（消耗的前缀 in 字符数），空则全部 0。
    // -----------------------------------------------------------------------
    std::vector<Candidate> Generate(const std::vector<SegPath>& paths,
                                    const std::string* inForCover, bool strictSep)
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
            // 纯声母简拼路径（如 dh、gylgl）：简拼价值在于命中整词（dh→电话、
            // gylgl→供应链管理）。长串纯声母不再整条丢弃——真简翻译词常 4~6
            // 声母，整丢=用户报"打到第 4 个声母词框消失"根因；但全跨度 span<n
            // 发射会出 x/y/z 单字垃圾墙（xyzxyz 误触），故长纯声母改走
            // "近全跨度"受限发射（见下方 longAllInit 块，与 tier 闸同构）。
            bool allInit = true;
            for (const auto& u : units)
                if (u.kind != SyllUnit::INITIAL) { allInit = false; break; }
            bool longAllInit = allInit && n >= 4;
            int initHead = trailingPartial ? n - 1 : n;
            if (trailingPartial)
            {
                bool prefixAllInit = true;
                for (int k = 0; k < n - 1; k++)
                    if (units[k].kind != SyllUnit::INITIAL) { prefixAllInit = false; break; }
                if (prefixAllInit && initHead >= 3) longAllInit = true;
            }

            // 逐单元回放在 in 上的消耗终点：撇号空转移跳过、单元文本按长度
            // 前进——与 EnumeratePaths 的 DP 转移严格一致，落点即 DP 位置。
            std::vector<int> unitEnd;
            if (inForCover && !inForCover->empty())
            {
                unitEnd.assign(n + 1, 0);
                int pos = 0;
                const std::string& inNorm = *inForCover;
                for (int k = 0; k < n; k++)
                {
                    while (pos < (int)inNorm.size() && inNorm[pos] == '\'') pos++;
                    // 零声母简写单元（B3）text 是改写后音节，长度≠原始键数，
                    // 回放前进必须用 rawLen，否则 cover 会越过原始输入末端。
                    pos += units[k].rawLen > 0 ? units[k].rawLen
                                               : (int)units[k].text.size();
                    unitEnd[k + 1] = pos;
                }
            }
            auto coverOf = [&](int k) -> int {
                return (int)unitEnd.size() > k ? unitEnd[k] : 0;
            };

            // 收集候选并记录覆盖单元数、词数与切分层，供分层排序。
            auto add = [&](std::wstring word, double cost, int covered, int words, int coverEnd)
            {
                if (!word.empty())
                    all.push_back({ std::move(word), cost, covered, n, words, tier, coverEnd });
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
                    mw[i][span] = MatchWords(sub, strictSep);
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

            // 长纯声母简拼（≥4 声母或残段前缀≥3 声母）：只做近全跨度（span≥n-1）
            // 与整词（span==n）词典发射；①②④ 与远跨度单字全部关闭——
            // gylg→供应链、gylgl→供应链管理（span==n 整词，用户主诉场景）；
            // xyzxyz 误触在 span 5/6 无词=零产出，不重建垃圾墙。
            // 旧实现在此整条 continue 丢弃：gylg/gylgl 一律 0 候选，
            // 词框消失（"打到第 4 个声母词框没了"根因）。
            if (longAllInit)
            {
                for (int span = n - 1; span <= n && span <= 6; span++)
                {
                    const std::vector<WordMatch>& ws = getMatches(0, span);
                    int added = 0;
                    int limit = (span == n) ? (int)kPredictMaxCandidates : 4;
                    for (const auto& w : ws)
                    {
                        if (added >= limit) break;
                        double uncovered = (double)(n - span) * 8.0;
                        double c = WordCost(w.idx) + w.penalty + kWordCountCost + uncovered;
                        add(_entries[w.idx].word, c, span, 1, coverOf(span));
                        added++;
                    }
                }
                continue;
            }

            // 碎切分路径（tier>0）只允许 ③词典词发射：①整句链/②首词变体/④残段
            // 拼接三类**跨单元字符拼接**依赖散字母单元（a→啊、n→嗯、o→哦）与
            // 前缀匹配自由组合，产出非词典垃圾（"西啊年/西安嗯/无哦/五嗯"），
            // tier 降层只压排序不阻产出，60 上限尾部仍成垃圾墙——用户报
            // "翻页大量无逻辑三字候选"实证（probe：xian 尾部西啊嗯系、un 尾部
            // 无嗯系、uo 尾部无哦系）。tier 0（自然切分）全段保留；tier 1 的
            // 真实词典词（[u][n]→"五年"类 span==n 整词）不受影响。
            if (tier != 0)
            {
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
                        add(_entries[w.idx].word, c, span, 1, coverOf(span));
                        added++;
                    }
                }
                continue; // ①②④ 拼接段对碎切分整体关闭
            }

            std::vector<Node> dp = computeDp(n);

            // 1) 整句最优链（覆盖全部 units）。纯声母简拼只接受"一个整词覆盖全部"，
            // 不做逐字声母硬拼（重码率极高，且 xyzxyz 之类应判为英文）。
            if (dp[0].valid && dp[0].firstIdx >= 0 &&
                !(allInit && dp[0].nWords != 1))
                add(reconstruct(dp, n, 0), dp[0].cost, n, dp[0].nWords, coverOf(n));

            // 2) 首词不同的整句变体（用于给出同拼音的其他整词）
            // 质量闸（用户实测"首页被单字替换垃圾变体刷屏"——进万/金丸/浸完类）：
            //   ① 词长偏好：首词覆盖音节数 < 最优链首词 → 每个短一截罚 1.5
            //      （jinwan：今晚[2音节] 压过 进[1音节]+晚上）；
            //   ② cost 上限：变体总 cost 超最优链 +2.5（≈首词频率低 12 倍）直接不出。
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
                    if (dp[0].firstSpan > span)
                        c += 1.5 * (double)(dp[0].firstSpan - span); // 词长偏好
                    if (c > dp[0].cost + 2.5) continue;              // 低频变体闸
                    if (allInit && nwords != 1) continue; // 纯简拼只给整词，不拼尾巴
                    add(s, c, n, nwords, coverOf(n));
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
                    add(_entries[w.idx].word, c, span, 1, coverOf(span));
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
                        add(head + _entries[ci].word, c, n, headWords + 1, coverOf(n));
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
        if (size == INVALID_FILE_SIZE) { CloseHandle(h); return {}; } // 防 4GB 误申请
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
