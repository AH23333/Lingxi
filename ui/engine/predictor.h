#pragma once
//
// 预测引擎接口。M3 = n-gram/词表（ngram.cpp）；M4 = llama.cpp 小模型（llm.cpp）。
// 引擎在 LingxiUI.exe 进程内运行，绝不接触宿主应用内存。
//
#include <string>
#include <vector>

namespace lingxi {

struct Prediction
{
    std::vector<std::wstring> cands; // ≤5 个，第 0 项为首选
    uint32_t replaceLen = 0;         // 上屏时需先删掉光标前的字符数（英文前缀补全用）
};

class IPredictor
{
public:
    virtual ~IPredictor() = default;
    // context = 光标前的原始文本（≤300 字符）。要求快速返回（>300ms 会被放弃）。
    virtual Prediction Predict(const std::wstring& context) = 0;
};

// 词表引擎（永远可用）
IPredictor* CreateDefaultPredictor();

// LLM 引擎：运行时加载 <exe目录>\llama\llama.dll + <exe目录>\models\*.gguf。
// 任一资源缺失 → 返回 nullptr（调用方自然落到词表引擎）。
IPredictor* CreateLlmPredictor();

// 装配降级链：LLM（若可用）→ 词表。返回值所有权归调用方。
IPredictor* CreatePredictorChain();

} // namespace lingxi
