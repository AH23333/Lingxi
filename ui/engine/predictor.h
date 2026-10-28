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
    // 拼音候选部分提交（B1）：与 cands 平行的"该候选消耗的原始拼音前缀字符数"。
    // 约定：size()==cands.size() 才有效；元素 0 = 未知/不适用（提交后全清，旧行为）。
    // 0 < cover < 串长 → 选该词后剩余拼音留在组合里继续输入（"白打"根治）。
    // 双拼模式引擎不提供（键位→全拼映射不可逆推），恒为空/0。
    std::vector<uint32_t> cover;
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

// 拼音候选重排器：根据已上屏上文，用 LLM 对候选做指令式重排。
// 与 LlmPredictor 共享同一份模型/上下文（不重复加载）。
class ILlmReranker
{
public:
    virtual ~ILlmReranker() = default;
    // committedCtx=已上屏上文；cands=拼音候选（原始顺序）。
    // 返回候选下标的重排（长度须等于 cands.size()）；失败/超时/未就绪返回空 = 保持原序。
    virtual std::vector<size_t> Rerank(const std::wstring& committedCtx,
                                       const std::vector<std::wstring>& cands,
                                       uint64_t deadlineMs) = 0;
    // 调用方（管道层）捕获 llama 内部硬件异常时熔断：本会话永久禁用 LLM
    // （不重启不重试），续写链自然落到词表引擎。默认空实现（非 LLM 实现无所谓）。
    virtual void MarkBroken() {}
    virtual bool Broken() const { return false; }
};

// 装配降级链：LLM（若可用）→ 词表。返回值所有权归调用方。
// sharedLlm 非空时复用该 LLM 实例（不取得所有权，由调用方保证其生命周期，
// 通常让返回的 chain 持有）；为空时内部自行创建。
IPredictor* CreatePredictorChain(IPredictor* sharedLlm = nullptr);

// 续写回显裁剪（B4）：小模型续写常以"复读上文尾部"开头，直接当候选上屏
// = 输入框出现完全重复的内容（用户实测报告）。裁掉输出与 tail（上文尾部）
// 重叠的前缀。升级点（B4）：空白/标点归一化比较（回显与尾部只差标点也命中）、
// 重叠上限抬到 tail 全长、整段复读循环剥干净；保留 k≥2 阈值（单字歧义）。
// 实现于 ngram.cpp（engine_test 直接单测）。
std::wstring StripContinuationEcho(std::wstring cont, const std::wstring& tail);

// 上下文感知降级链（实现于 ngram.cpp，engine_test 用假引擎单测路由）：
//   词中（尾部 ≥2 英文词符）→ 词表补全优先（replaceLen 语义正确）；
//   纯 ASCII 上下文（整段 < 0x80，B4）→ 词表优先；其余引擎兜底时候选
//   过滤为纯 ASCII（qwen 续写英文上下文会吐中文候选），全滤空则无候选；
//   其余（中文上下文/词边界）→ 按链序（LLM 优先）。
// chain 各元素所有权归返回的 predictor（析构时逐一 delete）。
IPredictor* CreateChainPredictor(std::vector<IPredictor*>&& chain);

} // namespace lingxi
