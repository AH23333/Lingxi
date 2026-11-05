// hotwords_logic.h —— 统计窗"常选词 TOP N"纯逻辑（离线热词，台账批2-①）。
// 数据源：用户词典计数列（userdict.txt，选词自学习累计次数）——这是本机
// 唯一真实存在的"选词热度"记录，不新增采集面。纯函数，engine_test 直钉。
#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace lingxi {

// userdict.txt 行式：<词>\t<规范化拼音键>\t<count>[\t<manual>[\t<lastUsed>]]
// 容错：缺列/坏行跳过；count<=0 跳过。返回按 count 降序、同数按词升序
// （确定序，测试可钉），至多 topN 条。
inline std::vector<std::pair<std::string, int>>
ParseTopUserWords(const std::string& raw, size_t topN)
{
    std::vector<std::pair<std::string, int>> all;
    size_t pos = 0;
    while (pos < raw.size())
    {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string line = raw.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos || t1 == 0) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        int count = std::atoi(line.c_str() + t2 + 1);
        if (count <= 0) continue;
        all.emplace_back(line.substr(0, t1), count);
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const auto& a, const auto& b) {
                         if (a.second != b.second) return a.second > b.second;
                         return a.first < b.first;
                     });
    if (all.size() > topN) all.resize(topN);
    return all;
}

} // namespace lingxi
