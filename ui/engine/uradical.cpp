#include "uradical.h"
//
// U 模式拆字实现（GAP §1）。数据与匹配自包含：静态策展表 → 惰性索引
// （key=部件读音去空格拼接；toks=读音序列，供"已拼出部件数"计分）。
// 匹配域全窄串 ASCII（小写化后），命中才转宽输出；索引只读，多线程安全。
//
#include <algorithm>

namespace lingxi {
namespace {

// 首批策展 196 条：目标字 + 一级直属部件读音（书写序，包围结构外框先）。
// 收录口径：部件归属与读音无争议（独体/叠字/合并字/族字）；读音有争议
// 不收（宁缺毋滥）。部件读音避 a/o 打头（B3 ua/uai/uan/uang/uo 短路保活）；
// 女作 nv；不成字部首用俗称（氵shui 亻ren 宀bao 辶zou 囗wei 疒bing 灬huo）。
struct RadEntry { const wchar_t* ch; const char* comps; };

const RadEntry kRadTable[] = {
    // —— 独体部件字（自身读音即键；兼作族字拼写中的早出口）——
    { L"金", "jin" }, { L"木", "mu" },   { L"水", "shui" }, { L"火", "huo" },
    { L"土", "tu" },  { L"人", "ren" },  { L"口", "kou" },  { L"日", "ri" },
    { L"月", "yue" }, { L"山", "shan" }, { L"石", "shi" },  { L"田", "tian" },
    { L"力", "li" },  { L"女", "nv" },   { L"子", "zi" },   { L"手", "shou" },
    { L"心", "xin" }, { L"大", "da" },   { L"小", "xiao" }, { L"王", "wang" },
    { L"立", "li" },  { L"足", "zu" },   { L"儿", "er" },   { L"耳", "er" },
    { L"尔", "er" },  { L"牛", "niu" },  { L"马", "ma" },   { L"羊", "yang" },
    { L"鱼", "yu" },  { L"鸟", "niao" }, { L"虫", "chong" },{ L"贝", "bei" },
    { L"车", "che" }, { L"门", "men" },  { L"页", "ye" },   { L"走", "zou" },
    { L"竹", "zhu" }, { L"广", "guang" },{ L"雨", "yu" },   { L"穴", "xue" },
    { L"米", "mi" },  { L"鹿", "lu" },
    // —— 三叠 ——
    { L"众", "ren ren ren" },   { L"森", "mu mu mu" },
    { L"晶", "ri ri ri" },      { L"淼", "shui shui shui" },
    { L"焱", "huo huo huo" },   { L"鑫", "jin jin jin" },
    { L"垚", "tu tu tu" },      { L"犇", "niu niu niu" },
    { L"骉", "ma ma ma" },      { L"猋", "quan quan quan" },
    { L"赑", "bei bei bei" },   { L"毳", "mao mao mao" },
    { L"羴", "yang yang yang" },{ L"麤", "lu lu lu" },
    { L"鱻", "yu yu yu" },      { L"品", "kou kou kou" },
    { L"磊", "shi shi shi" },   { L"惢", "xin xin xin" },
    { L"劦", "li li li" },      { L"叒", "you you you" },
    { L"皛", "bai bai bai" },   { L"龘", "long long long" },
    { L"矗", "zhi zhi zhi" },
    // —— 二叠 ——
    { L"朋", "yue yue" }, { L"林", "mu mu" },   { L"从", "ren ren" },
    { L"双", "you you" }, { L"昌", "ri ri" },   { L"炎", "huo huo" },
    { L"圭", "tu tu" },   { L"多", "xi xi" },   { L"吕", "kou kou" },
    { L"哥", "ke ke" },   { L"囍", "xi xi" },
    // —— 四叠 ——
    { L"燚", "huo huo huo huo" }, { L"叕", "you you you you" },
    { L"朤", "yue yue yue yue" },
    // —— 合并字 ——
    { L"甭", "bu yong" },  { L"孬", "bu hao" }, { L"歪", "bu zheng" },
    { L"尜", "xiao da xiao" }, { L"奀", "bu da" }, { L"巭", "gong fu" },
    // —— 水族 ——
    { L"汆", "ru shui" },  { L"氽", "ren shui" }, { L"泵", "shi shui" },
    { L"汞", "gong shui" },{ L"泉", "bai shui" },{ L"淦", "shui jin" },
    { L"沓", "shui yue" },
    // —— 火族 ——
    { L"灭", "yi huo" },   { L"灾", "bao huo" }, { L"烎", "kai huo" },
    { L"炆", "huo wen" },  { L"烨", "huo hua" }, { L"煜", "huo yu" },
    // —— 日族 ——
    { L"昊", "ri tian" },  { L"昕", "ri jin" },  { L"昱", "ri li" },
    { L"旻", "ri wen" },   { L"晟", "ri cheng" },{ L"暄", "ri xuan" },
    { L"晗", "ri han" },   { L"炅", "ri huo" },  { L"杲", "ri mu" },
    { L"明", "ri yue" },
    // —— 王族 ——
    { L"玥", "wang yue" }, { L"珏", "wang yu" }, { L"琪", "wang qi" },
    { L"琦", "wang qi" },  { L"琳", "wang lin" },{ L"玮", "wang wei" },
    { L"璟", "wang jing" },{ L"瑄", "wang xuan" },{ L"璇", "wang xuan" },
    // —— 女族 ——
    { L"好", "nv zi" },    { L"姝", "nv zhu" },  { L"妍", "nv kai" },
    { L"妤", "nv yu" },    { L"娴", "nv xian" }, { L"婷", "nv ting" },
    { L"婉", "nv wan" },   { L"嫣", "nv yan" },
    // —— 门族 ——
    { L"闯", "men ma" },   { L"闪", "men ren" }, { L"间", "men ri" },
    { L"问", "men kou" },  { L"闻", "men er" },  { L"闲", "men mu" },
    { L"闷", "men xin" },  { L"闹", "men shi" },
    // —— 囗族 ——
    { L"国", "wei yu" },   { L"回", "wei kou" }, { L"困", "wei mu" },
    { L"因", "wei da" },   { L"团", "wei cai" }, { L"园", "wei yuan" },
    { L"围", "wei wei" },  { L"固", "wei gu" },  { L"圆", "wei yuan" },
    { L"囚", "wei ren" },
    // —— 辶族 ——
    { L"边", "zou li" },   { L"达", "zou da" },  { L"迈", "zou wan" },
    { L"过", "zou cun" },  { L"运", "zou yun" }, { L"近", "zou jin" },
    { L"返", "zou fan" },  { L"送", "zou guan" },{ L"迷", "zou mi" },
    // —— 疒族 ——
    { L"疗", "bing le" },  { L"疯", "bing feng" },{ L"病", "bing bing" },
    { L"疼", "bing dong" },{ L"痒", "bing yang" },{ L"症", "bing zheng" },
    { L"疤", "bing ba" },
    // —— 广族 ——
    { L"床", "guang mu" }, { L"庙", "guang you" },{ L"府", "guang fu" },
    { L"店", "guang zhan" },
    // —— 亻族 ——
    { L"休", "ren mu" },   { L"你", "ren er" },  { L"仙", "ren shan" },
    { L"件", "ren niu" },  { L"付", "ren cun" },
    // —— 艹族 ——
    { L"萱", "cao xuan" }, { L"薇", "cao wei" }, { L"萌", "cao ming" },
    // —— 雨族 ——
    { L"雯", "yu wen" },   { L"霖", "yu lin" },
    // —— 钅族 ——
    { L"钰", "jin yu" },
    // —— 土族 ——
    { L"尘", "xiao tu" },  { L"坐", "ren ren tu" }, { L"堃", "fang fang tu" },
    // —— 木族 ——
    { L"杰", "mu huo" },   { L"杳", "mu ri" },
    // —— 心族 ——
    { L"忐", "shang xin" },{ L"忑", "xia xin" },
    // —— 鬼族 ——
    { L"魈", "gui xiao" }, { L"魅", "gui wei" },
    // —— 山族 ——
    { L"岚", "shan feng" },
    // —— 名字用字 ——
    { L"曌", "ming kong" },{ L"喆", "ji ji" },   { L"甦", "geng sheng" },
    { L"頔", "you ye" },
    // —— 散字 ——
    { L"骂", "kou kou ma" },    { L"夯", "da li" },
    { L"蛊", "chong min" },     { L"鲁", "yu ri" },
    { L"轰", "che shuang shuang" }, { L"槑", "dai dai" },
    { L"宇", "bao yu" },        { L"宝", "bao yu" },
};

struct RadBuilt
{
    std::wstring ch;                    // 目标字
    std::string key;                    // 部件读音去空格拼接（匹配键）
    std::vector<std::string> toks;      // 读音序列（"已拼出部件数"计分用）
};

const std::vector<RadBuilt>& RadIndex()
{
    static const std::vector<RadBuilt> idx = [] {
        std::vector<RadBuilt> v;
        v.reserve(sizeof(kRadTable) / sizeof(kRadTable[0]));
        for (const auto& e : kRadTable)
        {
            RadBuilt b;
            b.ch = e.ch;
            std::string tok;
            for (const char* p = e.comps; *p; ++p)
            {
                if (*p == ' ')
                {
                    if (!tok.empty()) { b.toks.push_back(tok); tok.clear(); }
                }
                else { b.key += *p; tok += *p; }
            }
            if (!tok.empty()) b.toks.push_back(tok);
            v.push_back(std::move(b));
        }
        return v;
    }();
    return idx;
}

// 小写化拆出后缀："u/U"+纯字母（空后缀/非字母 → false）
bool SplitQuery(const std::wstring& s, std::string& rest)
{
    if (s.empty() || (s[0] != L'u' && s[0] != L'U')) return false;
    rest.clear();
    for (size_t i = 1; i < s.size(); ++i)
    {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
        if (c < L'a' || c > L'z') return false;
        rest += (char)c;
    }
    return !rest.empty();
}

// 命中档位：0=精确（q==key），1=前缀（key 以 q 开头且更长），-1=未命中。
// spelled=已拼出的完整部件数（前缀档计分：q 内对齐的 token 数）。
int MatchTier(const RadBuilt& b, const std::string& q, size_t& spelled)
{
    spelled = 0;
    if (b.key == q) return 0;
    if (b.key.size() > q.size() && b.key.compare(0, q.size(), q) == 0)
    {
        size_t pos = 0;
        for (const auto& tok : b.toks)
        {
            if (pos + tok.size() > q.size()) break;
            spelled++;
            pos += tok.size();
        }
        return 1;
    }
    return -1;
}

} // namespace

bool IsRadicalQuery(const std::wstring& s)
{
    std::string rest;
    if (!SplitQuery(s, rest)) return false;
    size_t dummy = 0;
    for (const auto& b : RadIndex())
        if (MatchTier(b, rest, dummy) >= 0) return true;
    return false;
}

std::vector<std::wstring> RadicalCandidates(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::string rest;
    if (!SplitQuery(s, rest)) return out;
    struct Hit { const RadBuilt* b; int tier; size_t spelled; };
    std::vector<Hit> hits;
    for (const auto& b : RadIndex())
    {
        size_t spelled = 0;
        int tier = MatchTier(b, rest, spelled);
        if (tier >= 0) hits.push_back({ &b, tier, spelled });
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.tier != b.tier) return a.tier < b.tier;   // 精确命中在前
        return a.spelled > b.spelled;                    // 已拼出部件多者在前
    });
    for (const auto& h : hits)
    {
        out.push_back(h.b->ch);
        if (out.size() >= 32) break; // 上限防御（候选窗分页足够）
    }
    return out;
}

} // namespace lingxi
