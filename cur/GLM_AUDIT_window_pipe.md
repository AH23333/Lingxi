# GLM_AUDIT — 候选窗/管道退出链审计结果与分工(QC → 豆包)

> 审计范围:window.cpp 全文 + pipeserver 退出链 + e2e.cpp 全文(子代理穷尽审计,QC 复核)。
> **已由 QC 修掉**:window.cpp Shutdown 不再 DeleteCriticalSection;e2e 的 SetText 越界返回 TS_E_INVALIDPOS、AdviseSink 先 QI 后替换。

## P1(你随异步重排重构一起修,正在你手上)

**退出竞态:pipeserver 侧同一病灶** —— `Stop()` 等线程只给 2s 超时,client 线程从不 join(pipeserver.cpp:118-119 创建后立刻 CloseHandle);超时后 `Stop` 删 `PipeServer::_cs`(:59)、`Shutdown` 删 `CandWindow::_cs`(QC 已把后者改为不删),仍存活的 PredictProc/client 线程(Rerank 慢、SendCandidates 写阻塞、ApplyLearn 词典重载都可能 >2s)醒来就踩已删 CS。
建议(选一):① `Stop()` 永不 DeleteCriticalSection(进程将亡,泄漏无害)——最小改动;② predict/client 线程句柄保留 + INFINITE join。若你正在做异步重排(StartAsyncRerank),顺手把线程生命周期收口进去。

## P2(建议随重构顺手)

1. **同管道多线程 WriteFrame 无锁**(pipeserver.cpp:452/465/479):PredictProc 发 MSG_CANDS(大帧可超管道缓冲→重叠写)与 UI 线程 SendPick/SendInsertText 并发写同一句柄——PITFALLS #10 在 UI 侧的复发面。建议 ClientCtx 内加写锁或发送封送单线程;
2. **`_queue.clear()` 跨客户端丢请求**(:350-356):防抖取 back 后清全队,多宿主场景客户端 B 的请求被静默清掉。按客户端保留最新;
3. **RebuildFonts 管道线程删在用字体**(window.cpp:93-95):DeleteObject 对正被 UI 线程 DC 选中的字体会失败,代码无条件置空 → 句柄泄漏 + 指针跨线程换(无撕裂,观感问题)。建议字号/皮肤变化 PostMessage 到 UI 线程重建(QC 认领,若你不动 window.cpp);
4. **pipeserver.h:41,70-73 死声明**:`ClientCtx::gen` 与 `StartAsyncRerank` 无定义无调用——若是你异步重排的半成品请继续;若已放弃请删除。

## P2 备忘池(不急,QC 持有)

- window:兜底定位 MonitorFromRect 用全零 rect 选错显示器(应 MonitorFromPoint(_lastX,_lastY));toast 计时器隐藏后 `_visible` 不复位且吞候选;LayoutMetrics 每帧读注册表(应由 _fontSize 推导);每帧每行建删 brush;页归一化两套逻辑;
- e2e:WaitTabEaten 200ms 无条件重激活是"用 churn 对抗 churn"(应按 #18b 判据仅在失焦时重激活);VEH 崩溃文件无时间戳/轮转;重试壳 DeleteFile 未检查返回值;OnTextChange view cookie 不自洽;候选窗断言可被真实打字误触发(校验锚点 rect 即可);
- pipeserver:PredictProc 无"处理期间来了更新 ver 则丢弃"前置检查(仅幽灵窗闪现,可接受)。

## 已核对干净(摘要)

e2e:TestStore TSF 契约主干/RequestLock SEH/三明治断言真走了 GetFocus 顶层上下文/句柄管理;window:GDI 全配对无泄漏/WM_PAINT 锁纪律/hover 无风暴/NOACTIVATE 设计/pipeserver:ver 全链一致。

—— QC 代理,2026-10-28 02:2x
