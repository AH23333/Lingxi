# 关机前交接(2026-10-28 02:3x,QC 代理收尾)

> 电脑即将关机。**所有有价值状态已提交或写盘**;本文件是重开机后第一个要读的。

## 一、git 状态(HEAD `6433a3f`,全链验证绿)

- 今晚净增提交:QC 修复(39535f5/2490ed5/db50e36)→ 文档代更(bbd7b86)→ 字号收尾(7fbd31b)→ R3 前置(e328a7d,豆包)→ R5 脚本(e950b86)→ 卸载增强(13bf85b)→ u16 对齐(19def02/fb3cfd7)→ 皮肤(d719057)→ **快捷键 P0 修复(b51dbb4)** → 退出竞态+e2e 契约(de5b47f)→ **引擎审计修复(6433a3f)**
- 最后验证:构建 ✅ / engine_test 全 PASS(含新用例 jbtm/vjlt)✅ / e2e EXIT=0 ✅

## 二、工作区遗留:豆包的 R3 在制品(**未提交,原样保留**)

`llm.cpp / predictor.h / main.cpp / pipeserver.cpp/h` 的改动 = LLM 整句重排的半成品(约 174 行):
- 已实现:ILlmReranker 接口、指令式 prompt、白名单/排列校验、陈旧闸(≤400ms)、共享 LLM 实例
- **提交前必须处理**(详见 cur/GLM_REVIEW_rerank.md):P1-1 双 prompt 形状槽(当前每次重排清空 KV,毁掉续写增量复用)、P1-2 prefill 硬预算闸(否则慢机预测线程被卡秒级)、P1-3 Rerank 调用点 SEH + s_llmDisabled、P1-4 单候选跳过、删两处 rerank_diag.txt 调试残留
- pipeserver.h 有死声明(ClientCtx::gen/StartAsyncRerank)——是异步重排半成品,继续或删除
- 重开后第一件事:读 GLM_REVIEW_rerank.md → 修完 P1 → 构建 + engine_test + e2e 三连 → 提交

## 三、引擎审计剩余 backlog(已修 P0/2×P1/4×P2,详见巡视记录)

1. **P1 EnumeratePaths 截断**:Prune 的 K_POS=7 与 K_FINAL=14 死代码,长串残段路径被不稳定 sort 挤出——修法:完整/残段分池各留 7 + stable_sort;加长串回归用例
2. P2 繁体一对多(头发→頭髮 等词级例外表);P2 Learn 锁内磁盘 IO(防抖);P2 ngram 注释失真/死代码
3. 测试盲区 Top5:xi'an 切分竞争、双拼非常规输入(零声母/大写/撇号)、双拼模式自学习闭环、繁体一对多、超长混排

## 四、系统状态

- 注册表/环境变量:干净(设置键=健康值,LINGXI_LOG 已除,无孤儿注册)
- 工具链新增:`tools/regress.cmd`(一键回归)、`tools/setup_dumps.cmd`(崩溃转储,**待用户以管理员跑一次**)
- 用户已实测确认:Edge 输入正常;快捷键 P0 修复(b51dbb4)待用户复测 Ctrl+C/V 等
- 每 20 分钟 QC 巡视为会话级自动化,关机后自然停止;重开机后如需继续,重新建立

## 五、用户指示备忘

- 不要穷举同音词;非选词交互优先(符号面板/双拼/皮肤/字号均已落地)
- R2 ✅ / R3 进行中(见上)/ R4 基本完成(剩自定义短语)/ R5 剩 32 位 TIP + 签名(方案在 GLM_EXPLORATION_R2R5.md)
- 32 位应用打字验证方法:用 `C:\Windows\SysWOW64\notepad.exe`(方案 §R5)

—— QC 代理 收尾
