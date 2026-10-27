# R3 方案:LLM 整句重排 + 多路采样 + KV cache 复用

> 由 QC 代理的专项子代理产出;头文件事实全部核对自 `deps/llama/include/llama.h`(tag b11900,255 个 LLAMA_API 已确认在 llama.dll 导出表);模型参数实测解析自 GGUF 头。性能数字除注明"已核实"外均为需实测。**照做即可,勿再探索。**

## 0. 结论摘要

1. **b11900 里 `llama_kv_self_*` 已不存在**,现役 API 是 `llama_memory_seq_rm/seq_cp/...`——llm.cpp 现有绑定(llm.cpp:97-98)已是正确形态,直接沿用。
2. **`llama_sampler_*` 链在 b11900 完整可用**(llama.h:1453-1650),可绑定;按 PITFALLS #16 全部 `required=false`,缺失时降级贪心。
3. **整句重排有前置堵点**:拼音模式下 DLL 只发拼音字母串、不发上文(composition.cpp:495),重排没有"上文 300 字符"可用——**必须先改 DLL 侧 SendPinyinCtx + proto**。
4. 300ms 预算下,重排只做"指令式排序"(输出数字序,~8-15 token),候选截 top-8;必须带**运行时自动降级闸**。
5. 多路采样默认关(settings 开关);拼音模式下 LLM 生成不能进候选,多路只服务重排投票。
6. KV 复用沿用现有"token 最长公共前缀 + seq_rm"(llm.cpp:292-304),补:双 prompt 形状槽 + seq_rm 返回值检查 + 失败回退开关。

## 1. 现状调用链(全部已核实)

```
ImeCore.dll
  composition.cpp:495  SendPinyinCtx → SendCtx(pinyin, rc, CTX_F_PINYIN)  ← 拼音模式只发拼音串
  service.cpp:352      OnContextRead → SendCtx(text, rc, 0)               ← 非拼音发光标前 ≤300 字符
  editsession.cpp:28-40  CReadSession: 读 [光标-300, 光标),kCtxMaxChars=300 (service.h:22)
    ↓ MSG_CTX(proto.h:69-80)
LingxiUI.exe
  pipeserver.cpp:232-256  HandleCtx → EnqueuePredict(:258-282,每客户端最新,全局队列 ≤8 :11,277)
  pipeserver.cpp:290-380  PredictProc(唯一消费者线程)
      :299-317  120ms 防抖 → :323-326 分流 pinyin/engine → :332-356 裁剪 → :358-378 Show+SendCandidates
```

llm.cpp 关键位置:绑定表 `LlamaApi::Load` :102-160;`LazyInit` :192-256(n_ctx=512 :240,n_batch=256 :241);`Generate` :258-350;KV 复用 :292-308;贪心 :325-331;预算闸 :336(**prefill 首次 decode 不在闸内** :312-317);`SplitContinuation` :354-403;`ChainPredictor::Predict` :448-466;`_genMutex` :261,414;预热线程 :484-487。

吞吐基线(RESOURCES.md,llama-cli 实测,需本进程复测):prefill ≈216 t/s,gen ≈77 t/s → **384 token 全量 prefill ≈1.8s,远超预算 → KV 复用是生存项不是优化项**。

## 2. 整句重排

### 2.1 前置:DLL 补发上文
- `ime/composition.cpp:467-497`(SendPinyinCtx 的编辑会话 lambda):仿 editsession.cpp:28-40,从组合起点 Clone→Collapse(TF_ANCHOR_END)→ShiftStart(-300)→GetText 读出上文随拼音一起发;
- `common/proto.h:69-80`:`MsgCtx` 加 `uint32_t ctxChars`,body = POD + ctxChars 个 wchar + textChars 个 wchar;新增 `CTX_F_CTX_PREFIX=0x02`;
- `ui/pipeserver.cpp:232-256`:按 ctxChars 拆上文与拼音串,`PendingPredict` 加 ctx 字段(pipeserver.h:55-64);
- HandleCtx 按"帧长 vs ctxChars"防御解析(不符→按旧格式只取 text)。

### 2.2 接口(predictor.h:43 后新增;装配在 ui/main.cpp:172-176 包装 pinyinEngine,勿塞进 pinyin.cpp)
```cpp
class ILlmReranker {
public:
    virtual ~ILlmReranker() = default;
    // 返回候选下标重排序;失败/超时/未就绪返回空 = 保持原序
    virtual std::vector<size_t> Rerank(const std::wstring& committedCtx,
                                       const std::vector<std::wstring>& cands,
                                       uint64_t deadlineMs) = 0;
};
ILlmReranker* CreateLlmReranker();  // 与 LlmPredictor 共享 LlamaApi/_ctx
```
线程:由 PredictProc 调,与 Generate 共用 `_genMutex` 串行。**铁律:LLM 调用永不嵌在 `PinyinPredictor::_udMtx` 临界区内**(拼音 Predict 返回后锁已释放,包装器天然满足)。

### 2.3 Prompt(指令式,输出数字序)
```
<|im_start|>system
你是输入法候选排序器。根据上文从候选中选出最连贯的排序。<|im_end|>
<|im_start|>user
上文:{committedCtx 尾部 200 字符}
候选:1.今天 2.天数 3.田野 ... (top-8)
只输出一行数字,按最自然顺序排列候选编号,如:2 1 3<|im_end|>
<|im_start|>assistant\n
```
- 全 prompt ≤300 token(200 上文≈200 tok + 候选 8×~4 + 指令 ~40);输出 8-15 token;
- **白名单硬校验**:只接受 1..K 整数 token;任何越界/非数字/数量≠K/空 → 返回空=回退原序;LLM 文本永不进候选。

### 2.4 管线位置与预算(300ms,防抖 120ms 在前)
| 阶段 | 预算 |
|---|---|
| 拼音 Predict | ≤10ms(现状 ~1ms;测试必须关 LINGXI_LOG,见 PITFALLS #20) |
| 重排 prefill | ≤150ms(KV 命中时增量 60-80 token;**上下文段必须缓存住**,是最大变数,需实测收紧) |
| 重排生成 | ≤120ms(~9 token @77 t/s) |
| 解析+合并 | ≤20ms |

**自动降级闸(必须)**:prefill/gen 计 EMA 吞吐,`prefill<300 t/s 或 gen<60 t/s → 本会话关重排与多路`;前 3 次请求跳过重排做标定。低配机自动退化为现状。
可选二期:两阶段呈现(先原序 Show,重排完再 Show 同 ver 更新;DLL OnCandidates 需幂等确认)。

## 3. 多路采样

### 3.1 API(已核实 llama.h 行号)
chain_init :1453 / chain_add :1456 / init_greedy :1473 / init_dist :1476 / init_top_k :1480 / init_top_p :1483 / init_min_p :1486 / init_temp :1492 / sample :1650 / get_seed :1637 / chain_n 自检 :1470 附近。全部 `required=false` 加入 llm.cpp:128-148 绑定表;建链后 `llama_sampler_chain_n()==4` 自检;任一缺失 → `_samplerOk=false` 走现有 argmax。
序列:`chain_init({no_perf=true})` → add(top_k 40)→add(top_p 0.9)→add(temp 0.7)→add(dist seed);每请求 `llama_sampler_reset`;`llama_sampler_accept` 已隐含在 sample 内(:1647 注释)。

### 3.2 成本与触发
- 每多一路=该路全部 token 各自 decode;**`llama_batch_get_one` 的 seq 固定为 0**(llama.h:961-963),并行分叉需手工 batch+seq_cp——一期不做;
- 一期务实:贪心路跑完 → `seq_rm(0, forkPos, -1)` 回滚 → 采样路重跑;两路 12+12 token ≈310ms,**仅 KV 命中(prefill≈20ms)时塞得进预算**;
- 触发:`kvUsable && _hasPrev && common ≥ n×0.9 && 已耗时 ≤60ms && settings.llmMultiPath`;seed=hash(前缀) 固定,防候选闪烁。

### 3.3 合并
- 非拼音续写:贪心结果第 0 组,采样路经 SplitContinuation 切分追加,精确去重,总上限 kMaxCands=9(llm.cpp:43);
- **拼音模式(关键约束)**:拼音候选会被 DLL 整体替换组合串,**LLM 续写绝不可进候选**;多路只做 Borda 投票(两路排名均值,冲突取贪心序;top-1 不一致 → 保守回退拼音原序)。

## 4. KV cache 复用

### 4.1 API(llama.h 已核实)
`llama_memory_seq_rm` :764(**返回 false=部分删除失败,现有代码没查,必须补**)、`seq_cp` :773(一期不用)、`seq_pos_max` :816、`memory_clear` :755、`llama_get_memory` :593(已绑定)。`llama_kv_self_seq_rm` 在此版本不存在。

### 4.2 现方案评价
"token 最长公共前缀 + seq_rm"正确且自愈;缓存键**不需要哈希**——`_prevPrompt`(:415)逐位比较,n≤384 成本可忽略;天然处理退格/改字/重复;位置由 decode 自动跟踪。

### 4.3 必须补三处
1. **seq_rm 返回值检查**(llm.cpp:297、303):false → `_hasPrev=false` + `memory_clear(mem,true)`;
2. **双 prompt 形状槽**:`_prevPrompt` 改 `_slots[2]`(shape∈{续写,重排})——拼音重排与英文续写交替会互踢单槽缓存;
3. **decode 返回值语义**(llama.h:994-1005):`1`=槽不足 → 截短 prompt(≤n_ctx×0.8)+全量重置;`<0`/`2` → `memory_clear`。

### 4.4 全量重置清单
decode `<0/1/2`;get_logits NULL(已处理);seq_rm false;abort 触发;模型重建;**换客户端不是重置条件**(前缀比较天然覆盖)。

### 4.5 settings 开关
`llmKvReuse=true` / `llmRerank=true` / `llmMultiPath=false`(common/settings.h:9-24 加字段,settings.cpp 套路照抄,设置页加三项,每次请求实时读取)。

## 5. 并发与安全
- LLM 只有两个调用方:PredictProc 线程 + 启动预热线程,`_genMutex` 串行;R3 不新增线程;
- 与 `_udMtx` 无交集(pinyin.cpp:294 的三方:Predict 读 :222 / Learn 写 :762 / Reload :684;重排包装器在 Predict 返回后才碰 LLM);
- 陈旧闸:PredictProc 调重排前 `now - e.enqueuedAt > 400ms → 跳过重排`(enqueuedAt 已存在 pipeserver.h:64)。

## 6. 内存(GGUF 头实测)
`qwen2.block_count=24, embedding=896, head=14, kv_head=2` → **KV/token = 2×24×2×64×2B ≈ 12 KiB**。n_ctx 512→1024 代价 ≈6 MiB,**值得**(重排 prompt ≤300 token + 24 生成,1024 有余量);n_batch 保持 256;权重 491MB mmap 不变;总 RSS ~0.6-0.7GB 与现状同量级。

## 7. 风险与防线(浓缩)
| 风险 | 防线 |
|---|---|
| 重排 hallucination | 白名单硬校验,违规即回退原序 |
| prefill 无预算闸(llm.cpp:312-317) | `llama_set_abort_callback`(llama.h:1128,CPU 后端支持);**abort 后必须 memory_clear**(ubatch 可能部分入 memory);软闸+硬闸双层 |
| llama AV 带崩 LingxiUI | PredictProc 调 LLM 处包 `__try/__except`,异常 → 原子 `s_llmDisabled` 本会话只用词表,不重启不重试 |
| 符号缺失 | 新符号全部 required=false,单点关闭不整体降级 |
| 版本漂移(#16) | llama.dll 换版必须同步换 include;首跑 `llama_sampler_chain_n()` + `llama_version()` 写日志 |
| 重排抖动 | seed 固定;top-1 不一致回退;与原序 top-1 相同不触发二次 Show |
| 日志陷阱 | 300ms 预算测试必须关 LINGXI_LOG;重排路径不加逐 token 日志 |

## 8. 实施顺序(最短路径)
1. **DLL 补发上文**(§2.1)——无此步免谈;用 fakeclient 发带 ctxChars 的帧验证;
2. **KV 加固**(§4.3 + n_ctx 512→1024 llm.cpp:240),回归现有续写(e2e + 退格);
3. **ILlmReranker + 指令式重排**(§2.2-2.3)+ settings `llmRerank` + 陈旧闸;先单路贪心重排;
4. **abort 硬闸 + SEH 包裹 + 自动降级闸**(§7)——在采样之前做;
5. **多路采样**(§3),`llmMultiPath` 默认 false;
6. 压测口径:关 LINGXI_LOG,记录 PredictProc 耗时 p50/p95(pipeserver.cpp:328 已有日志),目标 p95 ≤ 450ms。

**需实测**:①本进程 prefill/gen 吞吐;②重排 prompt 实际 prefill 耗时(决定预算表成立与否);③abort_callback 在 b11900 CPU 后端的中断粒度;④n_batch 256 对吞吐影响。

—— QC 代理 R3 专项,2026-10-28
