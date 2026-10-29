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
#include "../../common/settings.h"

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
constexpr uint64_t kBudgetMs = 300;          // 续写生成软预算（逐 token 检查）
constexpr uint64_t kRrPrefillAbortMs = 500;  // 重排 prefill 硬闸（llama_set_abort）
constexpr uint64_t kRrGenReserveMs = 150;    // prefill 闸为生成阶段预留的预算
constexpr int kRrMaxGenTokens = 40;
constexpr size_t kMaxCands = 9;
constexpr size_t kPromptMaxTokens = 384;
constexpr uint32_t kRrCtxTokens = 640;       // 重排专用 context 的 n_ctx（prompt≤512+生成）
// 自动降级闸（R3 专文 §2.4）：实测吞吐低于阈值 → 本会话永久关闭重排。
constexpr float kMinPrefillTps = 300.0f;
constexpr float kMinGenTps = 60.0f;
constexpr int kRerankCalibSamples = 3;

// FNV-1a：prompt token 序列 → 采样 seed。同 prompt 同 seed（采样路输出稳定，
// 防候选闪烁）；prompt 变化 seed 跟着变（探索新分支）。恒非 0。
uint32_t HashTokens(const std::vector<llama_token>& t, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n && i < t.size(); i++)
    {
        h ^= (uint32_t)t[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

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
    void (*memory_clear)(llama_memory_t, bool) = nullptr;
    // 采样链（R3 多路采样）：全部可选，缺失 → 采样路自动禁用、贪心路不受影响
    llama_sampler_chain_params (*sampler_chain_default_params)() = nullptr;
    llama_sampler* (*sampler_chain_init)(llama_sampler_chain_params) = nullptr;
    void (*sampler_chain_add)(llama_sampler*, llama_sampler*) = nullptr;
    int32_t (*sampler_chain_n)(const llama_sampler*) = nullptr;
    llama_sampler* (*sampler_init_top_k)(int32_t) = nullptr;
    llama_sampler* (*sampler_init_top_p)(float, size_t) = nullptr;
    llama_sampler* (*sampler_init_temp)(float) = nullptr;
    llama_sampler* (*sampler_init_dist)(uint32_t) = nullptr;
    llama_token (*sampler_sample)(llama_sampler*, llama_context*, int32_t) = nullptr;
    void (*sampler_free)(llama_sampler*) = nullptr;

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
            { "llama_model_free",            (void**)&free_model,          false },
            { "llama_log_set",               (void**)&log_set,             false },
            { "llama_get_memory",            (void**)&get_memory,          false },
            { "llama_memory_seq_rm",         (void**)&memory_seq_rm,       false },
            { "llama_memory_clear",          (void**)&memory_clear,        false },
            { "llama_sampler_chain_default_params", (void**)&sampler_chain_default_params, false },
            { "llama_sampler_chain_init",    (void**)&sampler_chain_init,  false },
            { "llama_sampler_chain_add",     (void**)&sampler_chain_add,   false },
            { "llama_sampler_chain_n",       (void**)&sampler_chain_n,     false },
            { "llama_sampler_init_top_k",    (void**)&sampler_init_top_k,  false },
            { "llama_sampler_init_top_p",    (void**)&sampler_init_top_p,  false },
            { "llama_sampler_init_temp",     (void**)&sampler_init_temp,   false },
            { "llama_sampler_init_dist",     (void**)&sampler_init_dist,   false },
            { "llama_sampler_sample",        (void**)&sampler_sample,      false },
            { "llama_sampler_free",          (void**)&sampler_free,        false },
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

// KV 增量复用状态（每个 context 一份，_genMutex 保护）：
// 提示词前缀缓存 + 是否存在可用前缀。“双 prompt 形状槽”（R3 专文 §4.3-2）的
// 落地方式 = 续写与重排各持一个独立 llama_context（同一份模型权重只加载一次）：
// 物理缓存天然隔离，重排的 prefill/清空永远碰不到续写的前缀，反之亦然。
struct KvState
{
    std::vector<llama_token> prompt;
    bool hasPrev = false;
};

class LlmPredictor : public IPredictor, public ILlmReranker
{
public:
    LlmPredictor() = default;
    ~LlmPredictor()
    {
        if (_api.ok && _rrCtx) _api.free_ctx(_rrCtx);
        if (_api.ok && _ctx) _api.free_ctx(_ctx);
        if (_api.ok && _model) _api.free_model(_model);
        if (_api.hDll) FreeLibrary(_api.hDll);
    }

    Prediction Predict(const std::wstring& context) override
    {
        std::call_once(_initFlag, [this]() { LazyInit(); });
        if (!_ready || _broken) return {};
        return Generate(context);
    }

    // ILlmReranker：指令式重排。与续写共用 _genMutex（CPU 推理算力串行）。
    std::vector<size_t> Rerank(const std::wstring& committedCtx,
                               const std::vector<std::wstring>& cands,
                               uint64_t deadlineMs) override
    {
        std::call_once(_initFlag, [this]() { LazyInit(); });
        // 廉价前置闸：未就绪 / 已熔断 / 本会话吞吐不达标自动降级 / 请求已过期
        if (!_ready || _broken || _rerankOff.load(std::memory_order_relaxed)) return {};
        if (cands.size() < 2 || GetTickCount64() >= deadlineMs) return {};
        return DoRerank(committedCtx, cands, deadlineMs);
    }

    // ILlmReranker：调用方捕获到 llama 内部 AV 时熔断（本会话不再用 LLM；
    // 续写链经 Predict 的 _broken 检查自然落到词表引擎）。
    void MarkBroken() override { _broken.store(true); }
    bool Broken() const override { return _broken.load(); }

private:
    // 重排 context 的 prefill 硬闸：decode 内部按 ubatch 粒度回调，
    // 返回 true 即中断（llama.h:1128；b11900 CPU 后端支持）。
    static bool AbortCb(void* data)
    {
        auto* deadline = (std::atomic<uint64_t>*)data;
        uint64_t d = deadline->load(std::memory_order_relaxed);
        return d != 0 && GetTickCount64() >= d;
    }

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

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        int cores = (int)si.dwNumberOfProcessors;

        llama_context_params cp = _api.context_default_params();
        cp.n_ctx = 1024; // 续写 prompt ≤384 + 生成 24；KV 增量复用
        cp.n_batch = 256;
        cp.n_threads = cores > 3 ? cores - 1 : 2;
        cp.n_threads_batch = cp.n_threads;
        _ctx = _api.init_from_model(_model, cp);
        if (!_ctx)
        {
            LX_LOG(L"llm", L"context 创建失败");
            _failed = true;
            return;
        }

        // 重排专用 context：与续写共享模型但物理缓存完全隔离（双槽方案）。
        // prefill 带 abort 硬闸——慢机上未命中的整段 prefill 会顶破预算，
        // 超预算由 llama 内部按 ubatch 粒度中断（abort 后的部分状态整序清除）。
        llama_context_params rp = _api.context_default_params();
        rp.n_ctx = kRrCtxTokens;
        rp.n_batch = 256;
        rp.n_threads = cp.n_threads;
        rp.n_threads_batch = cp.n_threads;
        rp.abort_callback = &LlmPredictor::AbortCb;
        rp.abort_callback_data = &_rrAbortDeadline;
        _rrCtx = _api.init_from_model(_model, rp);
        if (!_rrCtx)
            LX_LOG(L"llm", L"重排 context 创建失败（重排禁用，续写不受影响）");
        else if (!_api.get_memory || !_api.memory_seq_rm)
        {
            // KV 管理 API 缺失 → 无法做前缀复用，重排的 prefill 成本不可控 → 禁用
            LX_LOG(L"llm", L"memory API 缺失（重排禁用）");
            _api.free_ctx(_rrCtx);
            _rrCtx = nullptr;
        }

        _ready = true;

        // 采样链自检（R3 多路采样）：四段链构建成功才开采样路；缺任一符号照旧走贪心
        if (_api.sampler_chain_default_params && _api.sampler_chain_init &&
            _api.sampler_chain_add && _api.sampler_chain_n &&
            _api.sampler_init_top_k && _api.sampler_init_top_p &&
            _api.sampler_init_temp && _api.sampler_init_dist &&
            _api.sampler_sample && _api.sampler_free)
        {
            llama_sampler* ch = MakeSampler(1u);
            if (ch)
            {
                _samplerOk = true;
                _api.sampler_free(ch);
            }
        }
        LX_LOG(L"llm", L"采样链%s", _samplerOk ? L"可用" : L"不可用（多路采样禁用）");

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
            tokens.erase(tokens.begin(), tokens.begin() + (n - (int)kPromptMaxTokens));
            n = (int)kPromptMaxTokens;
        }

        // ---- M4.5 KV 增量复用 ----
        // 打字是追加型操作：新 prompt 与上次通常共享长前缀。策略：
        //   1) 先弃掉上次生成的尾部（KV 只留上次 prompt）；
        //   2) 求公共前缀 common，截掉分叉部分；
        //   3) 只 decode tokens[common..n)（位置由 llama_decode 自动跟踪）；
        //   4) prompt 完全相同 → 缩 1 个 token 重算末位 logits。
        // 数学上与全量重评估等价（greedy 输出不变），代价从全量 prefill 降到 delta。
        // 任意一步不对劲 → 回退全量（hasPrev=false + 整序清除）。
        size_t common = 0;
        bool kvUsable = _api.get_memory && _api.memory_seq_rm;
        auto ResetKv = [&]() {
            _genKv.hasPrev = false;
            if (!kvUsable) return;
            llama_memory_t mem = _api.get_memory(_ctx);
            // 整序删除永不失败（llama.h:760 注释）；只清本 context 的序列，
            // 不存在跨 context 误伤（重排在 _rrCtx 上，物理缓存独立）。
            if (mem && _api.memory_seq_rm) _api.memory_seq_rm(mem, 0, 0, -1);
        };

        if (!_genKv.hasPrev || !kvUsable)
        {
            common = 0;
        }
        else
        {
            llama_memory_t mem = _api.get_memory(_ctx);
            // 1) 弃掉上次生成尾部；seq_rm 返回 false=部分删除失败 → KV 不可信
            if (!_api.memory_seq_rm(mem, 0, (llama_pos)_genKv.prompt.size(), -1))
            {
                ResetKv();
            }
            else
            {
                while (common < _genKv.prompt.size() && common < (size_t)n &&
                       _genKv.prompt[common] == tokens[common])
                    common++;
                if (common == (size_t)n && n > 0) common = n - 1; // 完全相同 → 重算末位
                // 2) 截分叉；失败 → 全量重评估
                if (common < _genKv.prompt.size() &&
                    !_api.memory_seq_rm(mem, 0, (llama_pos)common, -1))
                {
                    ResetKv();
                    common = 0;
                }
            }
        }

        std::string outUtf8;
        uint64_t t0 = GetTickCount64();
        llama_batch batch = (common < (size_t)n)
                                ? _api.batch_get_one(tokens.data() + common, n - (int)common)
                                : _api.batch_get_one(tokens.data() + n - 1, 1);
        for (int step = 0; step < kMaxGenTokens; step++)
        {
            int rcDecode = _api.decode(_ctx, batch);
            if (rcDecode != 0)
            {
                // decode 失败（1=KV 槽不足，2=abort，<0/其他=错误）：KV 状态不可信，
                // 彻底清空并标记下次全量重评估
                LX_LOG(L"llm", L"decode 返回 %d，清空 KV", rcDecode);
                ResetKv();
                break;
            }
            float* logits = _api.get_logits_ith(_ctx, -1);
            if (!logits) { _genKv.hasPrev = false; break; }
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
            _genKv.prompt.assign(tokens.begin(), tokens.begin() + n);
            _genKv.hasPrev = true;
        }

        std::wstring cont = Utf8ToWide(outUtf8.data(), (int)outUtf8.size());
        cont = StripContinuationEcho(cont, tail);
        pred.cands = SplitContinuation(cont);

        // ---- 多路采样（R3 §3，默认关）：贪心路跑完回滚 KV 再跑采样路，合并候选 ----
        // 门：设置开 + 采样链可用 + 本次 KV 高命中（common≥90%——未命中时全量
        // prefill 已贵，二路预算不保）+ 贪心耗时 ≤60ms（慢机自动让位）。
        // fork：seq_rm 到 prompt 末位前一格 → 重 decode 末位拿新鲜 logits →
        // 采样循环。采样尾留在 KV 也安全（下次入口 seq_rm(prompt.size(),-1) 弃掉）。
        Settings st = LoadSettings();
        if (st.llmMultiPath && _samplerOk && kvUsable &&
            common * 10 >= (size_t)n * 9 && GetTickCount64() - t0 <= 60)
        {
            llama_memory_t mem = _api.get_memory(_ctx);
            if (mem && _api.memory_seq_rm(mem, 0, (llama_pos)n - 1, -1))
            {
                llama_sampler* smpl = MakeSampler(HashTokens(tokens, (size_t)n));
                if (smpl)
                {
                    llama_token lastTok = tokens[(size_t)n - 1];
                    llama_batch lb = _api.batch_get_one(&lastTok, 1);
                    if (_api.decode(_ctx, lb) == 0)
                    {
                        std::string outUtf8B;
                        for (int step = 0; step < kMaxGenTokens; step++)
                        {
                            // sampler_sample 内部读 ctx 末位 logits：先确非空防断言
                            if (!_api.get_logits_ith(_ctx, -1)) break;
                            llama_token t = _api.sampler_sample(smpl, _ctx, -1);
                            if (_api.vocab_is_eog(_vocab, t)) break;
                            char piece[64] = {};
                            int np = _api.detokenize(_vocab, &t, 1, piece, sizeof(piece), false, true);
                            if (np > 0) outUtf8B.append(piece, np);
                            if (GetTickCount64() - t0 > kBudgetMs) break; // 与贪心共享总预算
                            llama_batch nb = _api.batch_get_one(&t, 1);
                            if (_api.decode(_ctx, nb) != 0) { ResetKv(); break; }
                        }
                        std::wstring contB = Utf8ToWide(outUtf8B.c_str(), (int)outUtf8B.size());
                        contB = StripContinuationEcho(contB, tail);
                        auto extra = SplitContinuation(contB);
                        size_t added = 0;
                        for (auto& s : extra)
                        {
                            if (pred.cands.size() >= kMaxCands) break;
                            bool dup = false;
                            for (auto& e : pred.cands)
                                if (e == s) { dup = true; break; }
                            if (!dup) { pred.cands.push_back(s); added++; }
                        }
                        LX_LOG(L"llm", L"多路采样合并 +%zu 候选", added);
                    }
                    else
                    {
                        ResetKv();
                    }
                    _api.sampler_free(smpl);
                }
            }
        }
        // 三重去重（归一相等/前缀堆叠/上文复读）——实测逐键候选里一半是
        // 另一半的加标点/加尾巴版，模型还会整段复读上文（--probe-typing 取证）
        pred.cands = DedupCompletions(pred.cands, tail);
        return pred;
    }

    // 指令式重排：构造 prompt → 生成数字序 → 白名单校验。
    // 失败/超时/校验不通过 → 返回空（保持原序）。LLM 文本永不进候选。
    std::vector<size_t> DoRerank(const std::wstring& committedCtx,
                                 const std::vector<std::wstring>& cands,
                                 uint64_t deadlineMs)
    {
        std::lock_guard<std::mutex> lock(_genMutex);
        const size_t K = min(cands.size(), (size_t)8);

        std::wstring tail = committedCtx.size() > 200
                                ? committedCtx.substr(committedCtx.size() - 200)
                                : committedCtx;
        std::wstring candList;
        for (size_t i = 0; i < K; i++)
        {
            wchar_t b[8];
            _snwprintf_s(b, _TRUNCATE, L"%zu.", i + 1);
            candList += b + cands[i] + L" ";
        }
        wchar_t tailInstr[64];
        _snwprintf_s(tailInstr, _TRUNCATE,
                     L"\n只输出一行数字,必须包含 1 到 %zu 的每个数字且每个恰好一次,用空格分隔,最自然的排在最前。"
                     L"例如 %zu 个候选时输出形如 2 1 3", K, K);
        std::wstring prompt =
            L"<|im_start|>system\n你是输入法候选排序器。根据上文从候选中选出最连贯的排序。<|im_end|>\n"
            L"<|im_start|>user\n上文:" + tail + L"\n候选:" + candList +
            tailInstr + L"<|im_end|>\n"
            L"<|im_start|>assistant\n";
        std::string promptUtf8 = WideToUtf8(prompt);

        std::vector<llama_token> tokens((int)kRrCtxTokens);
        int n = _api.tokenize(_vocab, promptUtf8.c_str(), (int)promptUtf8.size(),
                              tokens.data(), (int)tokens.size(), false, true);
        if (n <= 0)
        {
            // 缓冲区不足时返回负值（所需 token 数的相反数）：上文再截短一档重试
            int half = tail.size() > 100 ? (int)tail.size() - 100 : 0;
            if (n < 0 && half > 0)
            {
                tail = tail.substr(tail.size() - half);
                return DoRerankInner(tail, cands, K, deadlineMs);
            }
            return {};
        }
        return DoRerankInner(tail, cands, K, deadlineMs, &tokens, n);
    }

    // tail 已截好；tokens 非空时复用（重试路径为空，内部重新 tokenize）
    std::vector<size_t> DoRerankInner(const std::wstring& tail,
                                      const std::vector<std::wstring>& cands,
                                      size_t K, uint64_t deadlineMs,
                                      const std::vector<llama_token>* pre = nullptr,
                                      int preN = 0)
    {
        std::vector<llama_token> tokens;
        int n = 0;
        if (pre)
        {
            tokens = *pre;
            n = preN;
        }
        else
        {
            std::wstring candList;
            for (size_t i = 0; i < K; i++)
            {
                wchar_t b[8];
                _snwprintf_s(b, _TRUNCATE, L"%zu.", i + 1);
                candList += b + cands[i] + L" ";
            }
            wchar_t tailInstr[64];
            _snwprintf_s(tailInstr, _TRUNCATE,
                         L"\n只输出一行数字,必须包含 1 到 %zu 的每个数字且每个恰好一次,用空格分隔,最自然的排在最前。"
                         L"例如 %zu 个候选时输出形如 2 1 3", K, K);
            std::wstring prompt =
                L"<|im_start|>system\n你是输入法候选排序器。根据上文从候选中选出最连贯的排序。<|im_end|>\n"
                L"<|im_start|>user\n上文:" + tail + L"\n候选:" + candList +
                tailInstr + L"<|im_end|>\n"
                L"<|im_start|>assistant\n";
            std::string promptUtf8 = WideToUtf8(prompt);
            tokens.resize(kRrCtxTokens);
            n = _api.tokenize(_vocab, promptUtf8.c_str(), (int)promptUtf8.size(),
                              tokens.data(), (int)tokens.size(), false, true);
            if (n <= 0) return {};
        }

        // ---- 重排槽 KV 增量（独立 _rrCtx，物理上与续写互不驱逐）----
        // 连续键入共享 system+上文前缀，只有候选行变化 → 全量 prefill 变 delta。
        size_t common = 0;
        auto ResetRrKv = [&]() {
            _rrKv.hasPrev = false;
            llama_memory_t mem = _api.get_memory(_rrCtx);
            if (mem) _api.memory_seq_rm(mem, 0, 0, -1); // 整序删除；abort 残留一并清除
        };
        if (!_rrKv.hasPrev)
        {
            common = 0;
        }
        else
        {
            llama_memory_t mem = _api.get_memory(_rrCtx);
            if (!_api.memory_seq_rm(mem, 0, (llama_pos)_rrKv.prompt.size(), -1))
            {
                ResetRrKv();
            }
            else
            {
                while (common < _rrKv.prompt.size() && common < (size_t)n &&
                       _rrKv.prompt[common] == tokens[common])
                    common++;
                if (common < _rrKv.prompt.size() &&
                    !_api.memory_seq_rm(mem, 0, (llama_pos)common, -1))
                {
                    ResetRrKv();
                    common = 0;
                }
                if (common == (size_t)n)
                {
                    // prompt 与上一次完全一致（同上文+同候选行）：结果必然相同。
                    // 直接回退原序——候选窗本来就以原序先显示，省一次 LLM 计算。
                    return {};
                }
            }
        }

        // prefill 硬闸：min(请求绝对 deadline - 生成预留, 起点 + 500ms)
        uint64_t now = GetTickCount64();
        uint64_t abortAt = now + kRrPrefillAbortMs;
        if (deadlineMs > now + kRrGenReserveMs && (uint64_t)(deadlineMs - kRrGenReserveMs) < abortAt)
            abortAt = deadlineMs - kRrGenReserveMs;
        _rrAbortDeadline.store(abortAt);

        llama_batch preB = _api.batch_get_one(tokens.data() + common, (int)(n - common));
        uint64_t tPre = GetTickCount64();
        int rc = _api.decode(_rrCtx, preB);
        uint64_t preMs = GetTickCount64() - tPre;
        _rrAbortDeadline.store(0);
        if (rc != 0)
        {
            // 1=KV 槽不足 / 2=abort（部分 ubatch 已入 KV）/ <0=错误：整序清除
            ResetRrKv();
            if (rc == 2)
            {
                // prefill 被硬闸掐断 = 本机吞吐不足以支撑重排
                LX_LOG(L"llm", L"重排 prefill abort（%llu ms / %zu tok），本会话关闭重排",
                       (unsigned long long)preMs, n - common);
                _rerankOff.store(true);
            }
            RecordRerankSample(0, 0, 0, 0);
            return {};
        }

        // 自回归生成数字序（约束解码：集齐 K 个数字前禁止 eog，防止 LLM 提前收尾）
        std::string outUtf8;
        int genTokens = 0;
        auto CountNums = [](const std::string& s) -> int {
            int cnt = 0; bool inNum = false;
            for (char c : s)
            {
                if (c >= '0' && c <= '9') { if (!inNum) { cnt++; inNum = true; } }
                else inNum = false;
            }
            return cnt;
        };
        uint64_t t0 = GetTickCount64();
        for (int step = 0; step < kRrMaxGenTokens; step++)
        {
            if (GetTickCount64() > deadlineMs) break;
            float* logits = _api.get_logits_ith(_rrCtx, -1);
            if (!logits) break;
            int32_t nv = _api.vocab_n_tokens(_vocab);
            bool needMore = CountNums(outUtf8) < (int)K;
            int best = -1;
            for (int32_t v = 0; v < nv; v++)
            {
                if (needMore && _api.vocab_is_eog(_vocab, v)) continue;
                if (best < 0 || logits[v] > logits[best]) best = v;
            }
            if (best < 0) break;
            if (!needMore && _api.vocab_is_eog(_vocab, best)) break;
            char piece[64] = {};
            int np = _api.detokenize(_vocab, (llama_token*)&best, 1, piece, sizeof(piece), false, true);
            if (np > 0) outUtf8.append(piece, np);
            genTokens++;
            llama_token bt = best;
            llama_batch nb = _api.batch_get_one(&bt, 1);
            if (_api.decode(_rrCtx, nb) != 0) break;
        }
        uint64_t genMs = GetTickCount64() - t0;

        // 保存 prompt 供下次增量（与续写槽同构；生成尾由下次入口统一弃掉）
        _rrKv.prompt.assign(tokens.begin(), tokens.begin() + n);
        _rrKv.hasPrev = true;

        // EMA 吞吐自动降级（专文 §2.4）：连续标定样本后不达标 → 本会话关重排
        RecordRerankSample((float)(n - common), (float)preMs, (float)genTokens, (float)genMs);

        // 解析数字 + 白名单（规则见 ParseOrder）
        std::vector<size_t> res;
        if (!ParseOrder(outUtf8, K, res)) return {};

        // ---- 多路采样 Borda 投票（R3 §3.3，默认关）----
        // 门：设置开 + 链可用 + 本轮 prefill 是增量（≤64 token，全量时预算不保）
        // + 距 deadline 仍余 ~150ms（采样 10-15 token 的生成成本）。
        // 两路排名取 Borda 均值，并列取贪心路相对序；两路 top-1 不一致 →
        // 保守回退拼音原序（返回空）。采样失败/部分输出 → 只用贪心路结果。
        Settings st = LoadSettings();
        if (st.llmMultiPath && _samplerOk && (size_t)(n - (int)common) <= 64 &&
            GetTickCount64() + 150 < deadlineMs)
        {
            llama_memory_t mem = _api.get_memory(_rrCtx);
            if (mem && _api.memory_seq_rm(mem, 0, (llama_pos)n - 1, -1))
            {
                llama_sampler* smpl = MakeSampler(HashTokens(tokens, (size_t)n));
                if (smpl)
                {
                    llama_token lastTok = tokens[(size_t)n - 1];
                    llama_batch lb = _api.batch_get_one(&lastTok, 1);
                    if (_api.decode(_rrCtx, lb) != 0)
                    {
                        ResetRrKv();
                    }
                    else
                    {
                        std::string outUtf8B;
                        for (int step = 0; step < kRrMaxGenTokens; step++)
                        {
                            if (GetTickCount64() > deadlineMs) break;
                            // sampler_sample 内部读末位 logits：先确非空防内部断言
                            if (!_api.get_logits_ith(_rrCtx, -1)) break;
                            bool needMore = CountNums(outUtf8B) < (int)K;
                            llama_token t = _api.sampler_sample(smpl, _rrCtx, -1);
                            // eog：集齐前采到 = 采样路提前收尾（漏编号 ParseOrder 补原序）
                            if (_api.vocab_is_eog(_vocab, t)) break;
                            char piece[64] = {};
                            int np = _api.detokenize(_vocab, &t, 1, piece, sizeof(piece), false, true);
                            if (np > 0) outUtf8B.append(piece, np);
                            if (!needMore) break; // 数字已够，不再多采
                            llama_batch nb = _api.batch_get_one(&t, 1);
                            if (_api.decode(_rrCtx, nb) != 0) { ResetRrKv(); break; }
                        }
                        std::vector<size_t> res2;
                        if (ParseOrder(outUtf8B, K, res2))
                        {
                            std::vector<size_t> merged = BordaMerge(res, res2, K);
                            if (merged.empty() || merged.front() != res.front())
                            {
                                LX_LOG(L"llm", L"重排两路 top-1 不一致，回退原序");
                                return {};
                            }
                            res = merged;
                        }
                    }
                    _api.sampler_free(smpl);
                }
            }
        }
        return res;
    }

    // 吞吐样本记录 + 自动降级判定（_genMutex 保护）。prefillMs/genMs 为毫秒。
    // 失败样本（aborted，全 0）计入一次慢速惩罚。
    void RecordRerankSample(float preTok, float preMs, float genTok, float genMs)
    {
        if (_rerankOff.load(std::memory_order_relaxed)) return;
        _rrSamples++;
        if (preTok > 0 && preMs > 0.5f && genTok > 0 && genMs > 0.5f)
        {
            float preTps = preTok / preMs * 1000.0f;
            float genTps = genTok / genMs * 1000.0f;
            _emaPreTps = (_rrSamples == 1) ? preTps : _emaPreTps * 0.5f + preTps * 0.5f;
            _emaGenTps = (_rrSamples == 1) ? genTps : _emaGenTps * 0.5f + genTps * 0.5f;
        }
        else
        {
            _emaPreTps = _emaPreTps * 0.3f; // 失败样本：指数重罚
            _emaGenTps = _emaGenTps * 0.3f;
        }
        if (_rrSamples >= kRerankCalibSamples &&
            (_emaPreTps < kMinPrefillTps || _emaGenTps < kMinGenTps))
        {
            _rerankOff.store(true);
            LX_LOG(L"llm", L"吞吐不达标（prefill=%.1f gen=%.1f t/s），本会话关闭重排",
                   _emaPreTps, _emaGenTps);
        }
    }

    // 构建采样链：top_k(40) → top_p(0.9,1) → temp(0.7) → dist(seed)。
    // 链接管各 sampler 所有权（chain_add），末尾只需 free 链本身。
    // 任一段构建失败 / 链长 ≠4 → 返回空（调用方放弃采样路，贪心路不受影响）。
    llama_sampler* MakeSampler(uint32_t seed)
    {
        llama_sampler_chain_params cp = _api.sampler_chain_default_params();
        cp.no_perf = true;
        llama_sampler* ch = _api.sampler_chain_init(cp);
        if (!ch) return nullptr;
        llama_sampler* s1 = _api.sampler_init_top_k(40);
        llama_sampler* s2 = _api.sampler_init_top_p(0.9f, 1);
        llama_sampler* s3 = _api.sampler_init_temp(0.7f);
        llama_sampler* s4 = _api.sampler_init_dist(seed);
        if (!s1 || !s2 || !s3 || !s4)
        {
            // 未入链的逐个释放；已入链的由 chain free 兜底
            if (s1) _api.sampler_free(s1);
            if (s2) _api.sampler_free(s2);
            if (s3) _api.sampler_free(s3);
            if (s4) _api.sampler_free(s4);
            _api.sampler_free(ch);
            return nullptr;
        }
        _api.sampler_chain_add(ch, s1);
        _api.sampler_chain_add(ch, s2);
        _api.sampler_chain_add(ch, s3);
        _api.sampler_chain_add(ch, s4);
        if (_api.sampler_chain_n(ch) != 4)
        {
            _api.sampler_free(ch);
            return nullptr;
        }
        return ch;
    }

    // 解析模型输出的数字序：重复编号 / 空 → false（调用方回退原序）；
    // 漏编号（LLM 提前收尾）把缺失编号按原序追加末尾（仍优于整体回退）。
    static bool ParseOrder(const std::string& utf8, size_t K, std::vector<size_t>& res)
    {
        std::wstring order = Utf8ToWide(utf8.c_str(), (int)utf8.size());
        std::vector<bool> seen(K, false);
        bool dup = false;
        const wchar_t* q = order.c_str();
        while (*q)
        {
            while (*q && !iswdigit(*q)) q++;
            if (!*q) break;
            long v = 0; int digits = 0;
            while (iswdigit(*q)) { v = v * 10 + (*q - L'0'); q++; if (++digits > 3) break; }
            if (v >= 1 && (size_t)v <= K)
            {
                size_t x = (size_t)(v - 1);
                if (seen[x]) { dup = true; break; }
                seen[x] = true;
                res.push_back(x);
            }
        }
        if (dup || res.empty()) return false;
        for (size_t x = 0; x < K; x++)
            if (!seen[x]) res.push_back(x);
        return true;
    }

    // Borda 计分合并两路全排列：score[x] = Σ(K - 位置)，两路各贡献 1..K 分。
    // 并列取贪心路相对序（a 为基底 + stable_sort）。a 必须是 0..K-1 全排列
    //（ParseOrder 补漏后保证）。
    static std::vector<size_t> BordaMerge(const std::vector<size_t>& a,
                                          const std::vector<size_t>& b, size_t K)
    {
        std::vector<int> posA(K, -1), posB(K, -1);
        for (size_t i = 0; i < a.size() && i < K; i++) posA[a[i]] = (int)i;
        for (size_t i = 0; i < b.size() && i < K; i++) posB[b[i]] = (int)i;
        std::vector<int> score(K, 0);
        for (size_t x = 0; x < K; x++)
        {
            if (posA[x] >= 0) score[x] += (int)K - posA[x];
            if (posB[x] >= 0) score[x] += (int)K - posB[x];
        }
        std::vector<size_t> order = a; // 贪心序为基底：并列时保持贪心相对序
        std::stable_sort(order.begin(), order.end(),
                         [&score](size_t x, size_t y) { return score[x] > score[y]; });
        return order;
    }

// 续写候选切分（用户实测重做）：旧版把同一段输出按 2 字/词累积切成最多 5 个
// **递增前缀**做候选——观感"全是对同一句话的部分重复、越列越长"。现在最多两条：
//   ① 首个自然短句（≤16 字，遇标点截断）；② 完整句（≤32 字，超长回缩词边界）。
// 不再堆叠同源前缀；两候选重复时去重。
    static std::vector<std::wstring> SplitContinuation(const std::wstring& cont)
    {
        std::vector<std::wstring> cands;
        size_t lead = 0;
        while (lead < cont.size() && (cont[lead] == L' ' || cont[lead] == L'\n' ||
               cont[lead] == L'\r' || cont[lead] == L'\t' || cont[lead] == L'"' ||
               cont[lead] == L'\u201c' || cont[lead] == 0x201D))
            lead++;
        std::wstring body = cont.substr(lead);
        while (!body.empty() && (body.back() == L'\n' || body.back() == L'\r' || body.back() == L' '))
            body.pop_back();
        if (body.empty()) return cands;

        const size_t kShortMax = 16, kFullMax = 32;
        auto addCand = [&](const std::wstring& s)
        {
            if (s.empty()) return;
            for (const auto& c : cands)
                if (c == s) return;
            cands.push_back(s);
        };
        // 首个句读边界（中英标点统一；<2 字符的碎屑不算分句）
        const wchar_t* kStops = L"\u3002\uff0c\uff01\uff1f\u3001\uff1b\uff1a,.!?;:"; // 中英句读集合
        size_t cut = std::wstring::npos;
        for (size_t k = 0; k < body.size(); k++)
            if (k >= 2 && wcschr(kStops, body[k])) { cut = k; break; }
        auto clip = [&](size_t len) -> std::wstring
        {
            std::wstring s = body.substr(0, (std::min)(len, body.size()));
            while (!s.empty() && (s.back() == L' ' || wcschr(kStops, s.back())))
                s.pop_back();
            return s;
        };
        if (cut != std::wstring::npos && cut <= kShortMax)
            addCand(clip(cut));
        if (body.size() <= kFullMax)
            addCand(body);
        else
        {
            size_t len = kFullMax;
            size_t sp = body.rfind(L' ', len);
            if (sp != std::wstring::npos && sp >= 12) len = sp; // 英文别切半词
            addCand(clip(len));
        }
        if (cands.empty())
            addCand(clip((std::min)(kShortMax, body.size())));
        return cands;
    }

    LlamaApi _api;
    llama_model* _model = nullptr;
    llama_context* _ctx = nullptr;     // 续写 context（seq0 单序列）
    llama_context* _rrCtx = nullptr;   // 重排 context（双槽方案的物理隔离）
    const llama_vocab* _vocab = nullptr;
    bool _ready = false;
    bool _failed = false;
    std::once_flag _initFlag;
    std::atomic<bool> _broken{ false };      // llama 调用捕获到硬件异常：本会话熔断
    std::atomic<bool> _rerankOff{ false };   // 吞吐不达标的会话级自动降级

    // M4.5 KV 增量复用状态（_genMutex 保护）
    std::mutex _genMutex;
    KvState _genKv; // 续写槽（_ctx）
    KvState _rrKv;  // 重排槽（_rrCtx）
    std::atomic<uint64_t> _rrAbortDeadline{ 0 }; // 重排 prefill 硬闸（0=不中断）
    int _rrSamples = 0;
    float _emaPreTps = 0, _emaGenTps = 0;
    bool _samplerOk = false; // 采样链自检通过（R3 多路采样可用）
};

} // namespace

IPredictor* CreateLlmPredictor()
{
    return new LlmPredictor();
}

#endif // LINGXI_HAS_LLAMA

IPredictor* CreatePredictorChain(IPredictor* sharedLlm)
{
    std::vector<IPredictor*> chain;
#if defined(LINGXI_HAS_LLAMA)
    IPredictor* llm = sharedLlm ? sharedLlm : new LlmPredictor();
    chain.push_back(llm);
#endif
    chain.push_back(CreateDefaultPredictor());
    LX_LOG(L"llm", L"预测链装配: %d 级", (int)chain.size());

#if defined(LINGXI_HAS_LLAMA)
    // 模型加载提前到启动期（后台预热），避免首次按键背上 1-3 秒加载
    IPredictor* llmWarm = chain[0];
    std::thread([llmWarm]() { llmWarm->Predict(L""); }).detach();
#endif
    // 上下文感知路由（词表补全/纯 ASCII 过滤/LLM 优先）在 ngram.cpp 实现，
    // engine_test 用假引擎单测（B4）
    return CreateChainPredictor(std::move(chain));
}

} // namespace lingxi
