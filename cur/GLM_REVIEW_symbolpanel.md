# GLM_REVIEW — symbolpanel 在制品审查(质检代理 → 豆包)

> 审查对象:main 工作区未提交的符号面板改动(2026-10-28 00:16 时点)。
> 按 P0/P1/P2 分级;标 **[必修]** 的项在你提交前必须处理,否则 QC 代理会接管修复。

## P1-1 [必修] emoji 代理对被按单 wchar 切割(点击插出半个字符)
`ui/symbolpanel.cpp`:
- 第 38 行 `SymCount()` 用 `wcslen`,第 80 行 `wchar_t s[2] = { CurrentSymbols()[idx], 0 }`,第 132-140 行绘制循环 `i % kCols / kCell` ——全部按"1 格 = 1 wchar"工作;
- 但"表情"类目(第 23 行)的 😀😁😂🤣 等**全部是 UTF-16 代理对(2 个 wchar)**。后果:网格把一个 emoji 画成两格乱码、点击插入**孤立高代理**(宿主文档出现无效字符),PanelHeight 计算也翻倍错位;
- 修法(最小):把"符号序列"改成按**码点**遍历的辅助函数——
  ```cpp
  // 返回码点数;遍历时 IsSurrogatePair(sym[i], sym[i+1]) 则步进 2
  int SymCount() { /* 代理对算 1 */ }
  // 点击处:
  int off = 第 idx 个码点的 wchar 偏移, len = 代理对 ? 2 : 1;
  std::wstring s(sym + off, len);
  ```
  绘制循环同样按码点步进,cell 索引与点击换算保持一致。

## P1-2 [必修] 符号插入未抑制 OnEndEdit → 插入后误弹预测候选
`ime/service.cpp` `MarshalWndProc` 的 `WM_APP_LX_INSERT` 分支:直接 `svc->InsertTextAtSelection(*s)`,没有像 `PinyinCommitHanzi`/`InsertCandidate`/`CommitChinesePunct` 那样包 `_fSelfInsert = TRUE ... FALSE`。上屏后 OnEndEdit 不被抑制 → RequestContextRead → 刚插完符号就弹出补全候选窗。修法:
```cpp
if (svc && s)
{
    if (svc->PinyinActive()) svc->AbortCompositionState();
    svc->SetSelfInsertGuard();   // 或直接 svc->_fSelfInsert = TRUE(经友元/公开小方法)
    svc->InsertTextAtSelection(*s);
    svc->ClearSelfInsertGuard();
}
```
(`_fSelfInsert` 是 private,加一对公开的守卫方法最干净。)

## P1-3 [必修] 删除 `tmp\panel_diag.txt` 诊断文件写入
`ui/symbolpanel.cpp:190-196`:`_wfopen_s(..., L"tmp\\panel_diag.txt", L"w")` 用**相对路径**——LingxiUI 由 DLL 在宿主进程(资源管理器/Edge…)里 CreateProcess 拉起,**继承宿主 CWD**,诊断文件会散落到任意目录(可能 System32,可能无权限)。这是"垃圾文件污染"模式,与本项目事故史同类。诊断请走 `LX_LOG`(已有 LINGXI_LOG 门控),把这段 FILE 写入整段删掉;顺带检查 `ClosePanel`/`WM_DESTROY` 路径没有残留同类写文件(你已删了 panel_msg.txt,好)。

## P2(建议,不阻塞提交)
1. **字体句柄泄漏**:每次面板关闭后再打开都 `CreateFontW` 覆盖 `g_font`,旧 HFONT 不 DeleteObject。开一次泄一次。建议创建前 `if (g_font) DeleteObject(g_font);` 或只在首次创建。
2. **多显示器**:`SM_CXSCREEN/SM_CYSCREEN` 只按主屏钳位,副屏(负坐标)上面板位置可能越界;可用 `MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)` + `GetMonitorInfo` 的 rcWork 钳位。
3. `WM_KEYDOWN` 的 Esc 关闭是死代码:面板 `WS_EX_NOACTIVATE` 永远收不到键盘焦点,可以删(或注释说明保留原因)。
4. 表情类目如果想让系统字体渲染彩色 emoji,`DrawTextW` 默认灰度渲染即可用;不必改,但若将来要用 `colr`/`Segoe UI Emoji` 记得字体回退。
5. `SendInsertText` 沿用了 `SendPick` 的"锁外用 `_active`"模式,与现有约定一致,暂不要求改(既有竞态 QC 已记录在案)。

## 已检查、没问题的
- `WS_EX_TOPMOST | WS_EX_NOACTIVATE` 置顶不抢焦点的窗口样式正确(宿主 TSF GetFocus 不被打断);
- `main.cpp` 的 `[&server]` 捕获生命周期安全(消息循环在 wWinMain 内);
- `proto.h` 消息号分配(MSG_INSERT_TEXT=11、WM_APP_LX_INSERT=+3)无冲突;
- `pipeserver.cpp SendInsertText` 的帧组装/WriteFrame 200ms 超时与现有发送方一致;
- `pipeclient.cpp MSG_INSERT_TEXT` 的堆分配 wstring + PostMessage + Marshal 侧 delete 的所有权闭环正确(PostMessage 失败或无窗口时也 delete 了)。

—— QC 代理(主 Quality 线,持续监视中)
