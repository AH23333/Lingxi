//
// LLM 预测引擎（M4）：运行时动态加载 llama.cpp 与 Qwen 小模型 GGUF。
//
// 版本漂移防御（见 docs/PITFALLS.md #16）：
//  - 头文件用 __has_include 探测（deps 未就绪时本文件编译为空实现），
//    结构体布局必须来自与 llama.dll 相同 tag 的头文件
//  - 所有函数经 GetProcAddress 绑定，任一缺失 → 引擎整体降级
//  - 生成路径用最稳定的 llama_decode + llama_get_logits_ith + 自实现贪心
//
#include "predictor.h"

#include "../../common/log.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#if __has_include("../../deps/llama/include/llama.h")
#define LINGXI_HAS_LLAMA 1
#include "../../deps/llama/include/llama.h"
#endif

namespace lingxi {

#ifndef LINGXI_HAS_LLAMA

// deps/llama/include/llama.h 不存在：LLM 引擎不可用，返回 nullptr（n-gram 兜底）
IPredictor* CreateLlmPredictor()
{
    return nullptr;
}

#else // LINGXI_HAS_LLAMA

namespace {

constexpr int kMaxGenTokens = 24;
constexpr uint64_t kBudgetMs = 300;
constexpr size_t kMaxCands = 5;
constexpr size_t kPromptMaxTokens = 384;

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return path;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::wstring Utf8ToWide(const char* p, int len)
{
    if (len <= 0) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, p, len, NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, p, len, &w[0], n);
    return w;
}

// 动态绑定表。签名与 deps/llama/include/llama.h（与 llama.dll 同 tag）一致。
struct LlamaApi
{
    HMODULE hDll = nullptr;

    void (*backend_init)() = nullptr;
    llama_model_params (*model_default_params)() = nullptr;
    llama_model* (*model_load_from_file)(const char*, llama_model_params) = nullptr;
    llama_context_params (*context_default_params)() = nullptr;
    llama_context* (*init_from_model)(llama_model*, llama_context_params) = nullptr;
    const llama_vocab* (*model_get_vocab)(const llama_model*) = nullptr;
    int32_t (*tokenize)(const llama_vocab*, const char*, int32_t, llama_token*, int32_t, bool, bool) = nullptr;
    int32_t (*detokenize)(const llama_vocab*, const llama_token*, int32_t, char*, int32_t, bool, bool) = nullptr;
    llama_batch (*batch_get_one)(llama_token*, int32_t) = nullptr;
    int32_t (*decode)(llama_context*, llama_batch) = nullptr;
    float* (*get_logits_ith)(llama_context*, int32_t) = nullptr;
    int32_t (*vocab_n_tokens)(const llama_vocab*) = nullptr;
    bool (*vocab_is_eog)(const llama_vocab*, llama_token) = nullptr;
    void (*free_ctx)(llama_context*) = nullptr;
    void (*free_model)(llama_model*) = nullptr;

    bool ok = false;

    bool Load()
    {
        std::wstring dir = ExeDir() + L"\\llama";
        std::wstring dll = dir + L"\\llama.dll";
        // LOAD_WITH_ALTERED_SEARCH_PATH：让 ggml.dll 等依赖先在 llama.dll 同目录找
        hDll = LoadLibraryExW(dll.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hDll)
        {
            LX_LOG(L"llm", L"LoadLibrary llama.dll 失败 err=%u", GetLastError());
            return false;
        }
        struct Binding { const char* name; void** pfn; bool required; };
        Binding binds[] = {
            { "llama_backend_init",          (void**)&backend_init,        true },
            { "llama_model_default_params",  (void**)&model_default_params, true },
            { "llama_model_load_from_file",  (void**)&model_load_from_file, true },
            { "llama_init_from_model",       (void**)&init_from_model,      true },
            { "llama_context_default_params",(void**)&context_default_params, true },
            { "llama_model_get_vocab",       (void**)&model_get_vocab,     true },
            { "llama_tokenize",              (void**)&tokenize,            true },
            { "llama_detokenize",            (void**)&detokenize,          true },
            { "llama_batch_get_one",         (void**)&batch_get_one,       true },
            { "llama_decode",                (void**)&decode,              true },
            { "llama_get_logits_ith",        (void**)&get_logits_ith,      true },
            { "llama_vocab_n_tokens",        (void**)&vocab_n_tokens,      true },
            { "llama_vocab_is_eog",          (void**)&vocab_is_eog,        true },
            { "llama_free",                  (void**)&free_ctx,            true },
            { "llama_model_free",            (void**)&free_model,          true },
        };
        for (auto& b : binds)
        {
            *b.pfn = (void*)GetProcAddress(hDll, b.name);
            if (b.required && !*b.pfn)
            {
                LX_LOG(L"llm", L"符号缺失: %hs（llama.dll 版本与头文件不匹配？）", b.name);
                return false;
            }
        }
        ok = true;
        return true;
    }
};

// 老版本 llama.cpp 的改名前 API 兼容（llama_load_model_from_file 等），
// 由绑定表按需扩展；当前绑定全部失败时 CreateLlmPredictor 返回 nullptr。

class LlmPredictor : public IPredictor
{
public:
    LlmPredictor() = default;
    ~LlmPredictor()
    {
        if (_api.ok && _ctx) _api.free_ctx(_ctx);
        if (_api.ok && _model) _api.free_model(_model);
        if (_api.hDll) FreeLibrary(_api.hDll);
    }

    Prediction Predict(const std::wstring& context) override
    {
        std::call_once(_initFlag, [this]() { LazyInit(); });
        if (!_ready) return {};
        return Generate(context);
    }

private:
    void LazyInit()
    {
        if (!_api.Load())
        {
            _failed = true;
            return;
        }
        // 模型：<exeDir>\models\ 下第一个 .gguf
        std::wstring modelDir = ExeDir() + L"\\models";
        std::wstring pattern = modelDir + L"\\*.gguf";
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            LX_LOG(L"llm", L"未找到模型 (%ls)", pattern.c_str());
            _failed = true;
            return;
        }
        std::wstring modelPath = modelDir + L"\\" + fd.cFileName;
        FindClose(hFind);

        _api.backend_init();
        llama_model_params mp = _api.model_default_params();
        mp.n_gpu_layers = 0; // CPU 推理（预编译为 CPU 版）
        std::string pathUtf8 = WideToUtf8(modelPath);
        _model = _api.model_load_from_file(pathUtf8.c_str(), mp);
        if (!_model)
        {
            LX_LOG(L"llm", L"模型加载失败: %ls", modelPath.c_str());
            _failed = true;
            return;
        }
        _vocab = _api.model_get_vocab(_model);

        llama_context_params cp = _api.context_default_params();
        cp.n_ctx = 512;
        cp.n_batch = 256;
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        int cores = (int)si.dwNumberOfProcessors;
        cp.n_threads = cores > 3 ? cores - 1 : 2;
        cp.n_threads_batch = cp.n_threads;
        _ctx = _api.init_from_model(_model, cp);
        if (!_ctx)
        {
            LX_LOG(L"llm", L"context 创建失败");
            _failed = true;
            return;
        }
        _ready = true;
        LX_LOG(L"llm", L"LLM 引擎就绪: %ls", fd.cFileName);
    }

    Prediction Generate(const std::wstring& context)
    {
        Prediction pred;

        // Qwen chat 模板；上下文取尾部 200 字符防超窗
        std::wstring tail = context.size() > 200 ? context.substr(context.size() - 200) : context;
        std::wstring prompt =
            L"<|im_start|>system\n你是输入法续写引擎。根据用户给出的文本输出自然的续写内容，"
            L"只输出续写部分，不要解释，不要重复原文。<|im_end|>\n"
            L"<|im_start|>user\n继续这段文本：" + tail + L"<|im_end|>\n"
            L"<|im_start|>assistant\n";
        std::string promptUtf8 = WideToUtf8(prompt);

        std::vector<llama_token> tokens(kPromptMaxTokens + 64);
        int n = _api.tokenize(_vocab, promptUtf8.c_str(), (int)promptUtf8.size(),
                              tokens.data(), (int)tokens.size(), false, true);
        if (n <= 0) return pred;
        if (n > (int)kPromptMaxTokens)
        {
            // 保留尾部（含模板尾部与最近上下文）
            tokens.erase(tokens.begin(), tokens.end() - kPromptMaxTokens);
            n = (int)kPromptMaxTokens;
        }

        std::string outUtf8;
        uint64_t t0 = GetTickCount64();
        llama_batch batch = _api.batch_get_one(tokens.data(), n);
        for (int step = 0; step < kMaxGenTokens; step++)
        {
            if (_api.decode(_ctx, batch) != 0) break;
            float* logits = _api.get_logits_ith(_ctx, -1);
            if (!logits) break;
            int32_t nv = _api.vocab_n_tokens(_vocab);
            int best = 0;
            for (int32_t v = 1; v < nv; v++)
                if (logits[v] > logits[best]) best = v;
            if (_api.vocab_is_eog(_vocab, best)) break;

            char piece[64] = {};
            int np = _api.detokenize(_vocab, (llama_token*)&best, 1, piece, sizeof(piece), false, true);
            if (np > 0) outUtf8.append(piece, np);
            if (GetTickCount64() - t0 > kBudgetMs) break;
            batch = _api.batch_get_one((llama_token*)&best, 1);
        }

        std::wstring cont = Utf8ToWide(outUtf8.data(), (int)outUtf8.size());
        pred.cands = SplitContinuation(cont);
        return pred;
    }

    // 续写切分为 ≤5 个递增候选：候选 i = 前缀 + 前 i+1 段
    // 英文按词边界切，中文按 2 字切
    static std::vector<std::wstring> SplitContinuation(const std::wstring& cont)
    {
        std::vector<std::wstring> segments;
        size_t i = 0;
        while (i < cont.size() && (cont[i] == L' ' || cont[i] == L'\n' || cont[i] == L'\r' ||
                                   cont[i] == L'\t' || cont[i] == L'"' || cont[i] == L'“' || cont[i] == L'“'))
            i++;
        std::wstring body = cont.substr(i);
        // 截掉成对引号收尾等修饰
        while (!body.empty() && (body.back() == L'\n' || body.back() == L'\r'))
            body.pop_back();
        if (body.empty()) return {};

        bool hasCJK = false;
        for (wchar_t ch : body)
            if (ch >= 0x4E00 && ch <= 0x9FFF) { hasCJK = true; break; }

        if (hasCJK)
        {
            for (size_t p = 0; p < body.size() && segments.size() < kMaxCands; p += 2)
            {
                size_t len = std::min<size_t>(2, body.size() - p);
                segments.push_back(body.substr(0, p + len));
            }
        }
        else
        {
            // 英文：词边界切
            size_t start = 0;
            while (start < body.size() && segments.size() < kMaxCands)
            {
                size_t end = body.find(L' ', start);
                if (end == std::wstring::npos) end = body.size();
                if (end > start)
                    segments.push_back(body.substr(0, end));
                start = end + 1;
                if (end >= body.size()) break;
            }
            if (segments.empty()) segments.push_back(body);
        }

        std::vector<std::wstring> cands;
        cands.reserve(segments.size());
        for (auto& s : segments)
        {
            if (s.size() > 64) s.resize(64);
            cands.push_back(s);
        }
        return cands;
    }

    LlamaApi _api;
    llama_model* _model = nullptr;
    llama_context* _ctx = nullptr;
    const llama_vocab* _vocab = nullptr;
    bool _ready = false;
    bool _failed = false;
    std::once_flag _initFlag;
};

} // namespace

IPredictor* CreateLlmPredictor()
{
    return new LlmPredictor();
}

#endif // LINGXI_HAS_LLAMA

// ---- 降级链：LLM（若可用）→ 词表 ----
namespace {

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

IPredictor* CreatePredictorChain()
{
    std::vector<IPredictor*> chain;
    if (IPredictor* llm = CreateLlmPredictor())
        chain.push_back(llm);
    chain.push_back(CreateDefaultPredictor());
    LX_LOG(L"llm", L"预测链装配: %d 级", (int)chain.size());
    return new ChainPredictor(std::move(chain));
}

} // namespace lingxi
