# GLM_REVIEW — R3 重排器 WIP 审查(QC → 豆包,提交前必读)

> 审查对象:工作区未提交的 llm.cpp/predictor.h/pipeserver/main 改动(2026-10-28 02:1x 时点)。
> 总评:白名单校验/排列校验/KV 异常兜底/回退语义全部正确,**骨架合格**;但有 4 个 P1 必须处理后再提交,否则重排会反向劣化现有体验。

## P1-1 [必修] 每次重排清空整个 KV cache → 毁掉续写的增量复用
`llm.cpp DoRerank`:进入时 `ClearKv() + _hasPrev=false`,结束时再来一遍。后果:拼音重排与英文续写**互踢缓存**,重排一跑,下次英文续写就是全量 prefill(384 token ≈1.4s @216 t/s)——R3 专文 §4.3-2 的"双 prompt 形状槽"正是为了防这个。
修法(按专文):`_prevPrompt` 改 `_slots[2]`(shape∈{续写=0,重排=1}),各自保留前缀;重排只用槽 1,**绝不碰槽 0**;`ClearKv` 仅在槽内前缀失配时做。

## P1-2 [必修] prefill 无预算闸 → 预测线程可被卡 1 秒以上
当前 deadline 只管生成循环(250ms),**prefill 完全无闸**。无 KV 命中的重排 prefill ≈300 token,慢机上轻松破秒,期间 PredictProc 阻塞、候选窗冻结。
修法(专文 §7#2):`llama_set_abort_callback`(b11900 CPU 后端支持,llama.h:1128 已核实),deadline 传入回调;**abort 后必须 memory_clear**(ubatch 可能部分入 memory,llama.h:998-1002)。加上专文 §2.4 的 EMA 自动降级闸(吞吐不达标本会话关闭重排),低配机自动退化现状。

## P1-3 [必修] Rerank 调用点缺 SEH 防线
专文 §7#3:llama 内部 AV 不能带崩 LingxiUI。当前 `pipeserver.cpp` 里 `_reranker->Rerank(...)` 裸调。修法:`__try/__except` 包裹,异常 → 原子 `s_llmDisabled=true` 本会话永禁重排(不重启不重试),回退原序。

## P1-4 [必修] 单候选也调 LLM
`pipeserver.cpp` 门槛是 `!pred.cands.empty()`——K=1 时重排是"花 1.4s 让 LLM 回答 1",纯浪费。改 `pred.cands.size() >= 2`。

## P2(建议)
1. **调试残留 ×2**:`main.cpp` 的 rerank_diag.txt 一次性写入块、`llm.cpp DoRerank` 的 `Dlog` lambda(每次重排开关文件)——提交前整段删除,诊断走 LX_LOGV(带 LINGXI_LOG 门控);
2. `tokens(512)` 定容:中文 200 字 + 候选 + 指令 ≤380,安全;但建议容量 = min(需要, n_ctx×0.8) 防御未来 prompt 变长;
3. e.ctx 为空(旧格式帧)时 prompt 里"上文:"为空串——无碍但可跳过重排省一次调用(可选项);
4. `deadlineMs` 形参在 DoRerank 内与自算的 250ms 双闸并存,语义重复,二选一即可。

## 已核对、正确的
- 白名单:数字流解析 → 越界跳过 → `res.size()==K` + `seen[]` 排列校验,任何违规回退原序;LLM 文本永不进候选 ✓
- pipeserver 侧:`order.size()==k` 门、陈旧闸(≤400ms)、top-8 截取、剩余候选原序拼接 ✓
- `_genMutex` 串行(与续写互斥)、`call_once` 初始化、decode/get_logits 失败路径 ✓
- ILlmReranker 横向 dynamic_cast、共享 LLM 实例只加载一次、chain 接管所有权 ✓
- 重排块位置(裁剪前、ASCII 清空后)与 ver 语义 ✓

## 建议顺序
P1-4(一行)→ P1-3(SEH)→ P1-1(双槽)→ P1-2(abort 闸)→ 删调试残留 → 构建 + engine_test + e2e 三连 + `LINGXI_LOG=1` 下实测 `cur/GLM_EXPLORATION_R3_LLM.md` §8 的需实测清单。P1-1/P1-2 若时间紧,可先落 P1-3/P1-4 + 调试残留清理后提交,双槽/硬闸随后补——但**不清理调试残留不要提交**。

—— QC 代理,2026-10-28 02:15
