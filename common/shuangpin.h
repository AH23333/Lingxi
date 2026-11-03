#pragma once
//
// 双拼方案键位（纯函数，引擎与台架共用消费，避免表漂移）。
// 方案：0=小鹤（历史默认，行为不变） 1=自然码 2=微软双拼（与搜狗双拼键位相同）。
// 声母三方案一致：zh→V ch→I sh→U。
// 键位依据（2026-11-03 检索）：ulpb.app/schemes/ziranma-vs-microsoft——
// 自然码 ing→Y、ü→V；微软 ing→;、ü→Y；零声母：小鹤/自然码 a/o/e 重复+ah/eg，
// 微软 O 前缀（oa=a ol=ai oj=an oh=ang ok=ao oe=e oz=ei of=en og=eng or=er oo=o ob=ou）。
//
#include <cstdint>
#include <initializer_list>
#include <string>

namespace lingxi {

inline std::string SpInitial(char c)
{
    if (c == 'v') return "zh";
    if (c == 'i') return "ch";
    if (c == 'u') return "sh";
    return std::string(1, c);
}

inline bool SpIn(const std::string& s, std::initializer_list<const char*> set)
{
    for (const char* x : set)
        if (s == x) return true;
    return false;
}

// 韵母键 + 已解析声母 → 全拼韵母；歧义键按声母消解。
// scheme=0 为小鹤原表（逐字保留，防行为漂移）；1/2 共用自然码主表，
// 差异收敛在 'y' 与 ';' 两键。
inline std::string SpFinal(char c, const std::string& initial, uint32_t scheme)
{
    const bool gk = SpIn(initial, { "g", "k", "h", "zh", "ch", "sh" });
    const bool jqx = SpIn(initial, { "j", "q", "x" });
    const bool nl = SpIn(initial, { "n", "l" });
    if (scheme == 0)
    {
        switch (c)
        {
        case 'q': return "iu";
        case 'w': return "ei";
        case 'e': return "e";
        case 't': return nl ? "ve" : "ue";
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
        case 'o': return SpIn(initial, { "b", "p", "m", "f" }) ? "o" : "uo";
        case 's': return jqx ? "iong" : "ong";
        case 'k': return gk ? "uai" : "ing";
        case 'l': return gk ? "uang" : "iang";
        case 'x': return gk ? "ua" : "ia";
        case 'v':
            if (jqx) return "u"; // ju/qu/xu
            if (nl) return "v";  // nv/lv
            return "ui";
        }
        return "";
    }
    switch (c)
    {
    case 'q': return "iu";
    case 'w': return gk ? "ua" : "ia";
    case 'e': return "e";
    case 'r': return "uan";
    case 't': return nl ? "ve" : "ue";
    case 'y':
        if (gk) return "uai";
        if (scheme == 1) return "ing";
        // 微软：Y=ü。jqx/y 后全拼写作 u（ju/qu/xu/yu），nl 后写作 v（nv/lv）
        if (jqx || initial == "y") return "u";
        if (nl) return "v";
        return "v";
    case 'u': return "u";
    case 'i': return "i";
    case 'o': return SpIn(initial, { "b", "p", "m", "f" }) ? "o" : "uo";
    case 'p': return "un";
    case 'a': return "a";
    case 's': return jqx ? "iong" : "ong";
    case 'd': return gk ? "uang" : "iang";
    case 'f': return "en";
    case 'g': return "eng";
    case 'h': return "ang";
    case 'j': return "an";
    case 'k': return "ao";
    case 'l': return "ai";
    case ';': return scheme == 2 ? "ing" : ""; // 微软 ing 键；自然码该键无韵母
    case 'z': return "ei";
    case 'x': return "ie";
    case 'c': return "iao";
    case 'v':
        if (jqx || initial == "y") return "u"; // 自然码 ü 键：ju/qu/xu/yu
        if (nl) return "v";
        return "ui";
    case 'b': return "ou";
    case 'n': return "in";
    case 'm': return "ian";
    }
    return "";
}

// 零声母音节 (c1,c2) → 全拼韵母。
inline bool SpZeroFinal(char c1, char c2, std::string& out, uint32_t scheme)
{
    if (scheme == 2)
    {
        if (c1 != 'o') return false; // 微软零声母统一 O 前缀
        switch (c2)
        {
        case 'a': out = "a"; return true;
        case 'l': out = "ai"; return true;
        case 'j': out = "an"; return true;
        case 'h': out = "ang"; return true;
        case 'k': out = "ao"; return true;
        case 'e': out = "e"; return true;
        case 'z': out = "ei"; return true;
        case 'f': out = "en"; return true;
        case 'g': out = "eng"; return true;
        case 'r': out = "er"; return true;
        case 'o': out = "o"; return true;
        case 'b': out = "ou"; return true;
        }
        return false;
    }
    // 小鹤/自然码零声母表位一致（a/o/e 重复 + ah=ang eg=eng）
    if (c1 == c2 && (c1 == 'a' || c1 == 'o' || c1 == 'e'))
    { out = std::string(1, c1); return true; }
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
inline std::string ShuangpinToQuanpin(const std::string& sp, uint32_t scheme)
{
    // 输入卫生：小写化并只保留 a-z（与 NormKey 同口径）。大写对（JV）会让
    // 韵母键查表落空、撇号/空格参与两两配对会造成音节错位（ni hao→酿奥）。
    std::string in;
    in.reserve(sp.size());
    for (char ch : sp)
    {
        char c = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        // ';' 必须保留：微软双拼的 ing 韵母键（其余方案查表落空，无害）。
        if ((c >= 'a' && c <= 'z') || c == ';') in.push_back(c);
    }

    std::string out;
    size_t i = 0;
    for (; i + 1 < in.size(); i += 2)
    {
        char c1 = in[i], c2 = in[i + 1];
        std::string syll;
        bool zeroLead = (scheme == 2) ? (c1 == 'o')
                                      : (c1 == 'a' || c1 == 'o' || c1 == 'e');
        if (zeroLead)
            SpZeroFinal(c1, c2, syll, scheme);
        if (syll.empty())
        {
            std::string initial = SpInitial(c1);
            syll = initial + SpFinal(c2, initial, scheme);
        }
        out += syll;
    }
    if (i < in.size()) out += SpInitial(in[i]); // 残段
    return out;
}

} // namespace lingxi
