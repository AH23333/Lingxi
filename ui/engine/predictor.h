#pragma once
//
// 预测引擎接口。M3 = n-gram/词表（ngram.cpp）；M4 = llama.cpp 小模型（llm.cpp）。
// 引擎在 LingxiUI.exe 进程内运行，绝不接触宿主应用内存。
//
#include <string>
#include <vector>

namespace lingxi {

// 引擎一次返回的最大候选数（支持候选翻页；UI 按 kPageSize 分页）。
constexpr size_t kPredictMaxCandidates = 60;

struct Prediction
{
    std::vector<std::wstring> cands; // 按优先级排序，第 0 项为首选；最多 kPredictMaxCandidates
    uint32_t replaceLen = 0;         // 上屏时需先删掉光标前的字符数（英文前缀补全用）
};

class IPredictor
{
public:
    virtual ~IPredictor() = default;
    // context = 光标前的原始文本（≤300 字符）。要求快速返回（>300ms 会被放弃）。
    virtual Prediction Predict(const std::wstring& context) = 0;
    // 用户选择反馈：rawPinyin=用户输入的原始拼音字母串，word=选中的词。
    // 用于自学习/动态调频；默认空实现，仅拼音引擎覆盖。
    virtual void Learn(const std::wstring& /*rawPinyin*/, const std::wstring& /*word*/) {}
    // 重新加载用户词典（管理界面清空/导入后立即生效）；默认空实现。
    virtual void ReloadUserDict() {}
};

// 词表引擎（永远可用）
IPredictor* CreateDefaultPredictor();

// 拼音引擎（M5）：原始拼音字母串 → 汉字候选。
// 输入 "jintianqi" → 词序列候选（如"今天天气"）；支持末尾音节不完整（"jintianq"）。
// Prediction.replaceLen 恒为 0（DLL 用组合串长度整体替换，不依赖该字段）。
IPredictor* CreatePinyinPredictor();

// LLM 引擎：运行时加载 <exe目录>\llama\llama.dll + <exe目录>\models\*.gguf。
// 任一资源缺失 → 返回 nullptr（调用方自然落到词表引擎）。
IPredictor* CreateLlmPredictor();

// 装配降级链：LLM（若可用）→ 词表。返回值所有权归调用方。
IPredictor* CreatePredictorChain();

} // namespace lingxi
