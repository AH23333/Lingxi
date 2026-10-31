#include "calcnum.h"

#include <cmath>
#include <cwchar>
#include <algorithm>

namespace lingxi {
namespace {

const wchar_t* kDigU = L"零壹贰叁肆伍陆柒捌玖";
const wchar_t* kDigL = L"零一二三四五六七八九";

const wchar_t* kSecU[4] = { L"", L"拾", L"佰", L"仟" };  // 组内 10/100/1000 位
const wchar_t* kSecL[4] = { L"", L"十", L"百", L"千" };
const wchar_t* kGrp[4] = { L"", L"万", L"亿", L"万亿" }; // 4 位组单位（低→高）

bool IsCalcChar(wchar_t c)
{
    return (c >= L'0' && c <= L'9') ||
           c == L'+' || c == L'-' || c == L'*' || c == L'/' ||
           c == L'(' || c == L')' || c == L'.';
}

// 无符号整数数字串 → 中文（upper=大写壹贰叁/小写一二三）。
// 长度上限 16 位（万亿级），超限返回空；允许前导零；全零 → 零。
// 规则：四位分组（万亿/亿/万），组内 仟佰拾 位；零折叠——组内连续零出一、
// 组内尾零不出、整组零与低位组前导零合并为一个零（如 100000005 → 壹亿零伍）。
std::wstring IntToCn(const std::wstring& digits, bool upper)
{
    const wchar_t* dig = upper ? kDigU : kDigL;
    const wchar_t* const* sec = upper ? kSecU : kSecL;
    size_t nz = digits.find_first_not_of(L'0');
    if (nz == std::wstring::npos) return std::wstring(1, dig[0]); // 全零 → 零
    std::wstring d = digits.substr(nz);
    if (d.size() > 16) return L""; // 超出万亿级：不产中文（调用方跳过该候选）
    int n = (int)((d.size() + 3) / 4); // 4 位组数（1..4）
    std::wstring out;
    bool pendingZero = false; // 整组零已挂账，待下一非零组前补一个零
    for (int g = 0; g < n; g++)
    {
        int size = (g == 0) ? (int)d.size() - (n - 1) * 4 : 4;
        int start = (g == 0) ? 0 : (int)d.size() - (n - g) * 4;
        // 组内渲染（左→右；power=组内位权 3..0）
        std::wstring gout;
        bool emitted = false, zeroPending = false;
        for (int i = 0; i < size; i++)
        {
            int dv = d[start + i] - L'0';
            int power = size - 1 - i;
            if (dv != 0)
            {
                if (zeroPending) { gout += dig[0]; zeroPending = false; }
                gout += dig[dv];
                if (power > 0) gout += sec[power];
                emitted = true;
            }
            else if (emitted)
            {
                zeroPending = true; // 组内后段可能有非零，先记账
            }
        }
        if (gout.empty()) // 整组为零：挂账待出（若无更低非零组则永不消费）
        {
            if (!out.empty()) pendingZero = true;
            continue;
        }
        // 补零：前有内容且（挂账的整组零 或 本组前导零即组值 <1000）
        if (!out.empty() && (pendingZero || d[start] == L'0'))
            out += dig[0];
        pendingZero = false;
        out += gout;
        out += kGrp[n - 1 - g];
    }
    // 小写首位"一十"归约为"十"（10→十；110→一百一十不动）
    if (!upper && out.size() >= 2 && out.compare(0, 2, L"一十") == 0)
        out.erase(0, 1);
    return out;
}

// 无符号数字串（可含一个小数点，"12.34"/"12"/"0.5"）→ upper/lower；
// 小数部分逐位读（壹点叁肆）。点后为空/非法时按无小数处理。
void CnFromNumberStr(const std::wstring& s, std::wstring& upper, std::wstring& lower)
{
    size_t dot = s.find(L'.');
    std::wstring ip = (dot == std::wstring::npos) ? s : s.substr(0, dot);
    std::wstring fp = (dot == std::wstring::npos) ? std::wstring() : s.substr(dot + 1);
    upper = IntToCn(ip, true);
    lower = IntToCn(ip, false);
    if (fp.empty()) return;
    if (!upper.empty())
    {
        upper += L"点";
        for (wchar_t c : fp) upper += kDigU[c - L'0'];
    }
    if (!lower.empty())
    {
        lower += L"点";
        for (wchar_t c : fp) lower += kDigL[c - L'0'];
    }
}

// 人民币金额：intD 无符号整数串，frac2 恰 2 位（角分）。
// 整数部分超 16 位返回空（不产金额候选）。
std::wstring RmbFrom(const std::wstring& intD, const std::wstring& frac2)
{
    bool intZero = intD.find_first_not_of(L'0') == std::wstring::npos;
    std::wstring ip = intZero ? std::wstring() : IntToCn(intD, true);
    if (ip.empty() && !intZero) return L"";
    int j = frac2[0] - L'0', f = frac2[1] - L'0';
    if (intZero && j == 0 && f == 0) return L"零元整";
    std::wstring s;
    if (!ip.empty()) s = ip + L"元";
    if (j == 0 && f == 0)          s += L"整";                 // 12.00 → 壹拾贰元整
    else if (j == 0)                                            // 12.03 → 零叁分；0.03 → 叁分
    {
        if (!s.empty()) s += L"零";
        s += std::wstring(1, kDigU[f]) + L"分";
    }
    else if (f == 0)               s += std::wstring(1, kDigU[j]) + L"角"; // 12.30 → 叁角
    else                           s += std::wstring(1, kDigU[j]) + L"角" +
                                        std::wstring(1, kDigU[f]) + L"分"; // 12.34 → 叁角肆分
    return s;
}

// double → 可读串：|r|<1e16 且（近）整数 → 整数串；小数 → %.6f 去尾零；
// 超界/非有限 → 空（无候选，回显兜底）。1e16 上限 = 万亿级 16 位。
std::wstring FormatDouble(double r)
{
    if (std::isnan(r) || std::isinf(r) || std::fabs(r) >= 1.0e16) return L"";
    double rounded = std::round(r);
    if (std::fabs(r - rounded) <= 1e-6 * (1.0 + std::fabs(r)))
        return std::to_wstring((long long)rounded);
    wchar_t buf[64];
    _snwprintf_s(buf, _TRUNCATE, L"%.6f", r);
    std::wstring s(buf);
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos)
    {
        size_t last = s.find_last_not_of(L'0');
        if (last == dot) s.resize(dot);
        else if (last != std::wstring::npos) s.resize(last + 1);
    }
    return s;
}

// ---- 表达式递归下降求值（+ - * / 括号 小数；除零/语法错 → ok=false） ----
struct CalcParser
{
    const wchar_t* p = nullptr;
    bool ok = false;

    double ParseExpr()
    {
        double v = ParseTerm();
        while (ok)
        {
            if (*p == L'+') { p++; v += ParseTerm(); }
            else if (*p == L'-') { p++; v -= ParseTerm(); }
            else break;
        }
        return v;
    }
    double ParseTerm()
    {
        double v = ParseFactor();
        while (ok)
        {
            if (*p == L'*') { p++; v *= ParseFactor(); }
            else if (*p == L'/')
            {
                p++;
                double d = ParseFactor();
                if (!ok) return 0;
                if (d == 0.0) { ok = false; return 0; } // 除零：非法
                v /= d;
            }
            else break;
        }
        return v;
    }
    double ParseFactor()
    {
        if (!ok) return 0;
        if (*p == L'-') { p++; return -ParseFactor(); } // 一元负号
        if (*p == L'+') { p++; return ParseFactor(); }
        return ParsePrimary();
    }
    double ParsePrimary()
    {
        if (!ok) return 0;
        if (*p == L'(')
        {
            p++;
            double v = ParseExpr();
            if (ok && *p == L')') { p++; return v; }
            ok = false;
            return 0;
        }
        if (*p < L'0' || *p > L'9') { ok = false; return 0; }
        double v = 0;
        while (*p >= L'0' && *p <= L'9')
        {
            v = v * 10 + (*p - L'0');
            if (v > 1.0e300) { ok = false; return 0; } // 字面量溢出保护
            p++;
        }
        if (*p == L'.')
        {
            p++;
            if (*p < L'0' || *p > L'9') { ok = false; return 0; } // "12." 缺小数位
            double scale = .1;
            while (*p >= L'0' && *p <= L'9')
            {
                v += (*p - L'0') * scale;
                scale *= .1;
                p++;
            }
        }
        return v;
    }
};

void PushUnique(std::vector<std::wstring>& v, const std::wstring& s)
{
    if (s.empty()) return;
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

} // namespace

bool IsCalcQuery(const std::wstring& s)
{
    return s.size() >= 2 && (s[0] == L'v' || s[0] == L'V') && IsCalcChar(s[1]);
}

std::vector<std::wstring> CalcCandidates(const std::wstring& s)
{
    std::vector<std::wstring> out;
    if (!IsCalcQuery(s)) return out;
    std::wstring rest = s.substr(1);
    // 字符集整体校验（"v1a" 拒收；字母/中文不可能进 v 组合，防御即可）
    for (wchar_t c : rest)
        if (!IsCalcChar(c)) return out;

    // 纯数字分支：^[0-9]*(\.[0-9]+)?$（前导点 ".5"=0.5；尾随点 "5." 与含
    // 运算符/括号的串都归表达式分支）。旧实现"无点即纯数字"把 "(1+2)*3"
    // 误判为纯数字串（表达式全挂的根因）。
    bool plain = true;
    size_t dot = std::wstring::npos;
    for (size_t i = 0; i < rest.size(); ++i)
    {
        wchar_t c = rest[i];
        if (c == L'.')
        {
            if (dot != std::wstring::npos || i + 1 >= rest.size()) { plain = false; break; }
            dot = i;
        }
        else if (c < L'0' || c > L'9') { plain = false; break; }
    }
    if (plain && rest.find_first_of(L"0123456789") == std::wstring::npos) plain = false; // 裸 "."
    if (plain)
    {
        std::wstring upper, lower;
        CnFromNumberStr(rest, upper, lower);
        PushUnique(out, upper);
        PushUnique(out, lower);
        // 金额候选：小数 ≤2 位（"12.3" 补 "30" 角位）
        size_t fd = rest.find(L'.');
        std::wstring fp = (fd == std::wstring::npos) ? std::wstring() : rest.substr(fd + 1);
        if (fp.size() <= 2)
        {
            std::wstring f2 = fp;
            if (f2.empty()) f2 = L"00";
            else if (f2.size() == 1) f2 += L"0";
            PushUnique(out, RmbFrom((fd == std::wstring::npos) ? rest : rest.substr(0, fd), f2));
        }
        return out;
    }

    // 表达式分支
    CalcParser ps;
    ps.p = rest.c_str();
    ps.ok = true;
    double r = ps.ParseExpr();
    if (!ps.ok || *ps.p != L'\0') return out; // 语法错/尾部残字符
    std::wstring rs = FormatDouble(r);
    if (rs.empty()) return out; // 超界/非有限
    PushUnique(out, rs);
    // 结果的中文读法（负数/小数支持；金额不适用表达式结果）
    std::wstring abs = (rs[0] == L'-') ? rs.substr(1) : rs;
    std::wstring upper, lower;
    CnFromNumberStr(abs, upper, lower);
    if (rs[0] == L'-')
    {
        if (!upper.empty()) upper.insert(0, L"负");
        if (!lower.empty()) lower.insert(0, L"负");
    }
    PushUnique(out, upper);
    PushUnique(out, lower);
    return out;
}

} // namespace lingxi
