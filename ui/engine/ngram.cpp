#include "predictor.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lingxi {

namespace {

constexpr size_t kMaxCands = 5;

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
        // 统计尾部连续 CJK 数：中文上下文至少 1 字才建议
        size_t i = ctx.size();
        while (i > 0 && IsCjkChar(ctx[i - 1])) i--;
        size_t cjkRun = ctx.size() - i;
        if (cjkRun == 0 || _zh.empty()) return;

        wchar_t lastChar = ctx.back();

        // M3 策略（诚实定位：无共现语料，质量占位，M4 由 LLM 接管）：
        // 下一词建议 = 词频表头部的高频词（多为"了/是/在/和"类接续字词），
        // 跳过与尾字相同的单字，避免"……不错"→"不错"这类无意义候选。
        for (const auto& e : _zh)
        {
            if (e.word.size() == 1 && e.word[0] == lastChar) continue;
            pred.cands.push_back(e.word);
            if (pred.cands.size() >= kMaxCands) break;
        }
        // replaceLen=0：纯插入（中文续写不做替换）
    }

    std::vector<WordEntry> _en;
    std::vector<WordEntry> _zh;
};

} // namespace

IPredictor* CreateDefaultPredictor()
{
    return new WordEngine();
}

} // namespace lingxi
