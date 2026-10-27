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
#include <thread>
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
constexpr size_t kMaxCands = 9;
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
    HMODULE hGgml = nullptr;
    std::wstring dir; // llama 运行时目录

    void (*backend_init)() = nullptr;
    void (*backend_load_all)() = nullptr;   // 来自 ggml.dll（新版本必须显式加载后端）
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
    void (*log_set)(ggml_log_callback, void*) = nullptr;
    llama_memory_t (*get_memory)(const llama_context*) = nullptr;
    bool (*memory_seq_rm)(llama_memory_t, llama_seq_id, llama_pos, llama_pos) = nullptr;

    bool ok = false;

    bool Load()
    {
        LX_LOG(L"llm", L"Load: 开始绑定 llama.dll");
        dir = ExeDir() + L"\\llama";
        std::wstring dll = dir + L"\\llama.dll";
        // LOAD_WITH_ALTERED_SEARCH_PATH：让 ggml.dll 等依赖先在 llama.dll 同目录找
        hDll = LoadLibraryExW(dll.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hDll)
        {
            LX_LOG(L"llm", L"LoadLibrary llama.dll 失败 err=%u", GetLastError());
            return false;
        }
        LX_LOG(L"llm", L"Load: llama.dll OK");
        hGgml = LoadLibraryExW((dir + L"\\ggml.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hGgml)
        {
            LX_LOG(L"llm", L"LoadLibrary ggml.dll 失败 err=%u", GetLastError());
            return false;
        }
        LX_LOG(L"llm", L"Load: ggml.dll OK");
        backend_load_all = (void (*)())GetProcAddress(hGgml, "ggml_backend_load_all");
        if (!backend_load_all)
        {
            LX_LOG(L"llm", L"符号缺失: ggml_backend_load_all");
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
            { "llama_log_set",               (void**)&log_set,             false },
            { "llama_get_memory",            (void**)&get_memory,          false },
            { "llama_memory_seq_rm",         (void**)&memory_seq_rm,       false },
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

    // 静音 llama 内部日志（否则 model loader 刷屏 stdout）
    static void QuietLog(ggml_log_level, const char*, void*) {}
    void SilenceLogs()
    {
        if (log_set) log_set(QuietLog, nullptr);
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
        LX_LOG(L"llm", L"LazyInit 开始");
        if (!_api.Load())
        {
            _failed = true;
            return;
        }
        _api.SilenceLogs();
        _api.backend_init();

        // ggml 的后端扫描基于当前工作目录：临时切到 llama 目录，
        // 让 load_all 找到 ggml-cpu-*.dll（特性分发后端），扫完立即恢复
        wchar_t oldCwd[MAX_PATH] = L"";
        DWORD got = GetCurrentDirectoryW(MAX_PATH, oldCwd);
        SetCurrentDirectoryW(_api.dir.c_str());
        _api.backend_load_all();
        if (got) SetCurrentDirectoryW(oldCwd);
        LX_LOG(L"llm", L"后端加载完成");

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
        // M4.5：KV cache 增量复用需串行化（预热线程与预测线程共用 _ctx）
        std::lock_guard<std::mutex> lock(_genMutex);
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

        // ---- M4.5 KV 增量复用 ----
        // 打字是追加型操作：新 prompt 与上次通常共享长前缀。策略：
        //   1) 先弃掉上次生成的尾部（KV 只留上次 prompt）；
        //   2) 求公共前缀 common，截掉分叉部分；
        //   3) 只 decode tokens[common..n)（位置由 llama_decode 自动跟踪）；
        //   4) prompt 完全相同 → 缩 1 个 token 重算末位 logits。
        // 数学上与全量重评估等价（greedy 输出不变），代价从全量 prefill 降到 delta。
        // 任意一步不对劲 → 回退全量（_hasPrev=false + 清 cache）。
        size_t common = 0;
        bool kvUsable = _api.get_memory && _api.memory_seq_rm;
        if (_hasPrev && kvUsable)
        {
            llama_memory_t mem = _api.get_memory(_ctx);
            _api.memory_seq_rm(mem, 0, (llama_pos)_prevPrompt.size(), -1); // 弃生成尾部
            while (common < _prevPrompt.size() && common < (size_t)n &&
                   _prevPrompt[common] == tokens[common])
                common++;
            if (common == (size_t)n && n > 0) common = n - 1; // 完全相同 → 重算末位
            if (common < _prevPrompt.size())
                _api.memory_seq_rm(mem, 0, (llama_pos)common, -1); // 截分叉
        }
        else if (kvUsable)
        {
            _api.get_memory(_ctx); // 首次：ctx 新建即空，无需处理
        }

        std::string outUtf8;
        uint64_t t0 = GetTickCount64();
        llama_batch batch = (common < (size_t)n)
                                ? _api.batch_get_one(tokens.data() + common, n - (int)common)
                                : _api.batch_get_one(tokens.data() + n - 1, 1);
        for (int step = 0; step < kMaxGenTokens; step++)
        {
            if (_api.decode(_ctx, batch) != 0)
            {
                // decode 失败：KV 状态不可信，下次全量重评估
                _hasPrev = false;
                if (_api.get_memory && _api.memory_seq_rm)
                    _api.memory_seq_rm(_api.get_memory(_ctx), 0, 0, -1);
                break;
            }
            float* logits = _api.get_logits_ith(_ctx, -1);
            if (!logits) { _hasPrev = false; break; }
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

        // 保存 prompt 状态供下次增量（生成尾部由下次入口统一弃掉）
        if (n > 0)
        {
            _prevPrompt.assign(tokens.begin(), tokens.begin() + n);
            _hasPrev = true;
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

    // M4.5 KV 增量复用状态（_genMutex 保护）
    std::mutex _genMutex;
    std::vector<llama_token> _prevPrompt;
    bool _hasPrev = false;
};

} // namespace

IPredictor* CreateLlmPredictor()
{
    return new LlmPredictor();
}

#endif // LINGXI_HAS_LLAMA

// ---- 降级链（上下文感知）----
//   光标处于英文单词中间（前缀 ≥2 字符）→ 词表补全优先（replaceLen 语义正确）
//   词边界 / 中文上下文 → LLM 续写优先；全部无结果 → 无候选
namespace {

bool IsEnWordChar(wchar_t ch)
{
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'0' && ch <= L'9') || ch == L'_';
}

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
        size_t i = context.size();
        while (i > 0 && IsEnWordChar(context[i - 1])) i--;
        bool midWord = (context.size() - i) >= 2;

        if (midWord && _chain.size() > 1)
        {
            // _chain[0]=LLM，_chain[1]=词表：词中用词表补全
            Prediction pred = _chain[1]->Predict(context);
            if (!pred.cands.empty()) return pred;
        }
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
#if defined(LINGXI_HAS_LLAMA)
    IPredictor* llm = new LlmPredictor();
    chain.push_back(llm);
#endif
    chain.push_back(CreateDefaultPredictor());
    LX_LOG(L"llm", L"预测链装配: %d 级", (int)chain.size());

#if defined(LINGXI_HAS_LLAMA)
    // 模型加载提前到启动期（后台预热），避免首次按键背上 1-3 秒加载
    std::thread([llm]() { llm->Predict(L""); }).detach();
#endif
    return new ChainPredictor(std::move(chain));
}

} // namespace lingxi
