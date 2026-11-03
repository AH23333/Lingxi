# 设计稿 C：候选窗横/竖排（"竖排候选"专题）

- 状态：**设计稿，待拍板**。全部结论基于 2026-11-03 对当前工作区源码的**静态阅读**，未做构建/运行验证；渲染效果类判断未经截图实测。
- 基线：HEAD `917b5a7`（并行会话在改 `ime/service.cpp`、`ui/main.cpp`、`ui/tray.cpp`，引用行号以当前磁盘状态为准）。

## 0. 先纠正前提（本次复核的最重要发现）

**任务前提"候选框无竖排/候选窗横向"与当前代码不符。** 实测：当前候选窗**已经是竖排列表**——每个候选独占一行、自上而下堆叠（[candwnd_render.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/candwnd_render.cpp#L302-L348) L302-L348：`row = i - pgBegin`，行矩形 `pad + compH + row*itemH`）；窗口高度 = 行数 × itemH（[candwnd_core.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/candwnd_core.cpp#L137) L137）。基线文档 L70"全库无竖排实现"是 grep"竖排"无命中得出的，**代码里没有的是"横排"和"方向切换"**，不是"竖排"。

因此与搜狗的真实差距是：**搜狗默认横排单行候选条、可切竖排；本项目只有竖排、无切换**。本稿按"横/竖排切换"为主体设计（§2 起）；若需求实为"竖排文字（旋转字形的古籍式排版）"，见 §9 单独结论（不建议做）。

## 1. 现状实测（file:line 均现读复核）

**协议层（分页硬编码）**
- `kPageSize = 9`（[proto.h](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/proto.h#L30) L30）、`kMaxCandidates = 60`（L32）、`NextPageStart` 翻页算术（L36-L44）；`MsgPage.start` 仅起始下标、**无 pageSize 字段**（L115-L119）；协议版本 `kProtoVer = 1`（L21）。
- 向后兼容先例：`MsgCtxV1` 定长旧帧（L89-L96）、`MsgCands.cover` 可选尾字段（L104-L107）——新增可选字段有成熟模式可抄。

**DLL 侧键盘（与渲染方向无关，全部按下标）**
- 拼音态键盘处理 `composition.cpp` L1100-L1220：数字 1-9 → `idx = _pageStart + (key-'1')`（L1198-L1207）；翻页键 PgUp/PgDn/`+`/`-`/↑/↓/Home/End（L1115-L1144，Home/End 末页算术在 service.cpp L559）；←/→ 被吞防跑光标（L1145-L1148）；空格选首（L1149-L1164）；未映射键放行前吞（L1210-L1219，钉死 deep_test T14）。
- **结论：键位语义 = 页内下标，与横/竖排解耦**——切换方向不需要动 DLL。

**UI 侧候选窗**
- 度量：`itemH = fontSize×15/8`（96dpi 基准，candwnd_render.cpp L81）；`fontSize` clamp 12..32（L26-L27）；DPI = `GetDpiForSystem()`（[candwnd_internal.h](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/candwnd_internal.h#L14-L21) L14-L21，**系统 DPI，非 per-monitor**）。
- 尺寸：宽 = max(200, 最长词 textMax+注音+标签+2×pad)（candwnd_core.cpp L143-L155）；高 = pad×2+compH+itemH×页内数+工具条（L137）。
- 屏幕适配：caret 中心启发 + 放不下翻上（L203-L204）；x/y 钳进工作区（L206-L207）；多屏用 `MonitorFromPoint + rcWork`（L189-L195，负坐标可用）。**无水平翻转**——窗口贴右缘时只钳不翻（横排长条会顶到右缘被截断，见 §5）。
- 命中测试：`row = (pt.y - yOff)/itemH`（[candwnd_interact.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/candwnd_interact.cpp#L184) L184、L497、L617、L754 四处）——横排需改为列命中。
- 超长词已有 toast 分支（candwnd_render.cpp L198-L222，>20 字整行呈现）。

**候选数语义（已收口，勿混用）**
- `candidateCount` 只管英文补全（[settings.h](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.h#L17-L18) L17-L18 注释、pipeserver.cpp L570 分叉、settingsui.cpp L1184 文案，commit `b43418d` 收口）；拼音每页恒 9。基线 §六-1（L167）"每页候选数可配"是**另一个独立待拍板项**，本稿仅在 §7 说明耦合。

**测试钉死现状**
- engine_test：`head=kPageSize` 页边界（L583）、"N>kPageSize 时第二页"（L775-L794）、**PIN 断言 `kPageSize==9`**（L2348-L2355）。
- deep_test：T12 依赖"ceshi 11 条 >9"翻第二页数字键选词（L1167-L1198）；T14 部分上屏（L1227-L1284）。**两者都走键盘路径，与 UI 渲染方向无关**，横/竖切换不破坏它们；但任何"pageSize 可配"改动会同时打碎这组基线。

## 2. 目标行为

**一句话体验**：设置里（或候选窗工具条/快捷键）一键切换候选窗横排/竖排，横排=搜狗式单行候选条贴着光标，竖排=现在的列表形态；切换立即生效、重启保持。

**明确不做**：
- 不做竖排文字（旋转字形，§9）。
- 不做按应用记忆方向、不做鼠标拖拽改方向。
- 第一期不做每页候选数可配（与方向切换解耦，独立裁决，见 §7）。

## 3. 数据模型与文件格式

- settings 新增键 `UI.candOrientation`：`0`=竖排列表（**缺省，保持现行为零变化**）、`1`=横排单行。旧版本读到未知键自动忽略（现有 settings 机制）；新版本读旧文件缺省 0 = 与今天逐像素一致。
- **无新文件、无词库格式变化、无协议字段**（第一期）：方向纯 UI 渲染态，DLL 不需要知道（键位=下标语义，§1 已证）。
- 唯一例外见 §7：横排"屏宽不够显示 fewer 候选"若要求数字键 7-9 与可见项严格一致，需 UI→DLL 回推可见数（新可选消息，按 MsgCands.cover 尾字段先例扩展）。

## 4. 横排布局度量（设计值，需真机校准）

- 单行高度 = `itemH`（复用 L81 公式），窗口高 = pad×2 + compH + itemH（无行数项）；工具条仍在底部（形态不变）。
- 宽度 = Σ(标签"1 "+词宽+注音宽+分隔 6px) 逐项累加，**上限 = rcWork宽 − 2×gap**（candwnd_core.cpp L188 的 gap 现值 4）。
- 逐项绘制：横排下注音只能放词下方（现竖排注音在词右侧，L338-L347）→ 横排行高需容纳两行字（fs + 注音 fs×0.72），即横排 `itemH_h = fs×15/8` 不够，设计为 `max(fs×15/8, fs + fs×0.72 + pad/2)`。此为纸面推算，**必须真机截图校准**。
- 词宽截断：单候选显示宽 cap = 8 字（超出省略号，横排无 toast 整行分支可用——toast 是竖排独占形态，L198-L222 在横排下禁用，右键完整预览保留）。
- 屏幕边缘：横排长条贴右缘时，现逻辑只钳 x（L206）→ 候选尾部被裁。设计：横排模式下先试 caret 左对齐，若 `x+w > rcWork.right` 则**右缘对齐翻转**（`x = rcWork.right - w`）；竖排维持现状。多屏负坐标：`MonitorFromPoint` 已按 caret 点取屏（L189-L195），负坐标天然可用，无需新逻辑。
- DPI：沿用 `GetDpiForSystem`（candwnd_internal.h L19）——混合 DPI 多屏下横排长条宽度会有缩放误差，竖排现形态同样有此局限（非本功能引入，标注为已知局限；per-monitor DPI 化是独立大项，不在本稿）。

## 5. 失败与降级

| 场景 | 行为 |
|---|---|
| candOrientation 值非法 | clamp 回 0（竖排），对齐 candidateCount 的 1..9 clamp 先例（settings.cpp L187） |
| 横排下 rcWork 窄于最小可行宽（如分屏极窄） | 自动回退竖排渲染（不报错、不空白），并在下次 LayoutAndPosition 重估 |
| 单候选超 cap（8 字） | 省略号截断 + 悬停 tooltip/右键预览（复用现有预览通道），**禁止整项丢弃** |
| 字号放大到 32 + 9 项横排超屏 | 同上右缘翻转 + 截断；若仍放不下 → 回退竖排 |

## 6. 原子性与断电安全

- 本功能只写 settings（现有原子写机制），无词库文件触碰，无新增落盘路径 → 断电面与现状相同。

## 7. 与"每页候选数可配"（基线 §六-1）的耦合

- 若把 `kPageSize` 做成可配（1..9），影响面完全不同：协议需带 pageSize（MsgCands 尾字段先例）+ DLL 翻页算术（composition.cpp L1115-L1144、service.cpp L559、proto.h L36-L44）+ **打碎 engine_test L2348-L2355 PIN 断言与 deep_test T12 的 >9 前提**。
- 方向切换（本稿）与 pageSize 可配（§六-1）**建议拆开裁决**：方向=纯 UI 低风险；pageSize=协议+DLL+测试基线三层联动高风险。

## 8. 测试设计与影响面清单

**台架（engine_test 可加）**
1. settings 读回：candOrientation 缺省 0 / 合法 1 / 非法值 clamp；旧文件无键 → 0。
2. 横排宽度计算纯函数（若把度量算式从 OnPaint 抽出成可测函数）：9 项×典型词宽、cap 截断、右缘翻转坐标断言。

**必须真机（渲染类无台架 harness）**
1. 两方向截图对比：100%/125%/150% DPI 下横排行高/注音两行/截断省略号；
2. 屏幕边缘：caret 贴右缘（横排右翻转）、贴下缘（两方向翻上，L203-L204 回归）、多屏负坐标副屏；
3. 交互回归：横排下悬停/单击/右键命中列序与数字键一致（interact 四处行命中 L184/L497/L617/L754 改列命中后）；滚轮翻页（L410-L413）语义不变；
4. deep_test T12/T14 全量跑（预期不受影响——键位走下标，与渲染无关；跑前注意 PITFALLS #92 双实例假红）。

**影响文件清单**
- `ui/candwnd_render.cpp`（OnPaint 方向分支、横排逐项绘制、toast 分支禁用条件）
- `ui/candwnd_core.cpp`（LayoutAndPosition 高度/宽度/右缘翻转）
- `ui/candwnd_interact.cpp`（命中测试行→列）
- `common/settings.h/.cpp`（新键 + clamp）
- `ui/settingsui.cpp`（切换行；注意 AGENTS.md"settingsui.cpp 只减不增"纪律——b43418d 已在此加行，新增入口需确认走既有行工厂还是新面板文件）
- 不动：`common/proto.h`、`ime/*`（第一期）、`ui/engine/*`

## 9. 若"竖排"实指竖排文字（旋转字形）——不建议做

GDI 旋转 90° 文本（`CreateFontW` 的 `escapement/orientation`）技术可行，但：注音 ruby 布局（L338-L347）在竖排文字下完全重做、命中测试/悬停/右键全链路重写、数字键视觉对应关系（古籍右起 vs 左起）引入新歧义、搜狗本身也不提供该形态。**收益不明、成本 ≥8 人日、与现有渲染架构冲突**——不建议，除非有明确用户诉求证据。

## 10. 风险与工作量

| 项 | 级别 | 粗估 |
|---|---|---|
| candOrientation 设置 + 横排渲染 + 右缘翻转 + 命中测试 | 低-中（纯 UI，不动协议/DLL/引擎） | 3~4 人日 |
| 真机 DPI/多屏/边缘验证 | 低（剧本可部分脚本化） | 1~2 人日 |
| 横排可见数回推 DLL（若要求数字键严格对齐可见项） | 中（协议新可选消息 + UI 重算） | +2 人日 |
| pageSize 可配（§六-1，独立项） | **高**（协议+DLL 翻页算术+测试基线联动） | 4~6 人日 |

## 11. 三段式结论

- **改什么**：先纠正前提——现状已是竖排列表；实际交付 = settings 新键 `candOrientation` + 横排单行渲染分支（candwnd_render/core/interact 三文件）+ 右缘翻转/宽度回退降级；不动协议、不动 DLL、不动引擎；pageSize 可配拆为独立裁决项。
- **风险**：最大风险是横排度量全靠纸面推算（行高/注音两行/cap 宽度），未经真机截图校准，落地首件是截图验证而非编码；其次是 interact 四处命中测试改列命中时的错位（悬停高亮与数字键不一致 = 用户直接踩空）。
- **不改的代价**：候选窗形态单一，与搜狗默认横排条的观感差异持续存在；但**功能正确性无缺口**（键位/翻页/选词已完整）——这是三项"重"功能里最接近"锦上添花"的一项，优先级可由用户直接压后。

## 12. 需要拍板的问题

1. **前提确认**：需求是"横/竖排切换"（本稿主体，现状=竖排列表）还是"竖排文字"（§9，不建议）？
2. 缺省方向：保持竖排（零行为变化，推荐）还是改横排对齐搜狗默认？
3. pageSize 可配（§六-1）是否与本项拆开、另行裁决？
4. 横排屏宽不足时：接受"回退竖排渲染"的降级，还是必须"少显示几个"（后者需 +2 人日回推 DLL）？
5. 切换入口：仅设置窗，还是候选窗工具条也要一个方向按钮？（F 键快捷键搜狗有，本项目键位表无空闲键，默认不做）

## 13. 裁决 2026-11-03（用户拍板，本节为唯一权威，与上文冲突处以本节为准）

1. **需求语义**：横排条 ↔ 竖排列表切换（§0 纠错成立）；**旋转字形竖排文字砍掉**（§9 结论采纳：成本≥8 人日、搜狗无此形态）。
2. **默认方向**：**保持竖排**（缺省值 0 不变）。理由（用户补充）：当前无法注销重登刷新已加载 DLL 的宿主，不允许在用户未看到的情况下改变默认外观。
3. **屏宽不足**：回退竖排渲染（§5 降级表采纳）；不做"少显示几个 + 可见数回推 DLL"（+2 人日路线砍掉）。
4. **pageSize 可配**：拆为独立高风险项，**不并入本功能**（§7 采纳；基线 §六-1 继续单独挂账）。
5. 切换入口（原 Q5）未裁决 → 按本稿默认执行：**仅设置窗**，候选窗工具条不加按钮、不加 F 键。

第一期改动面锁定：`ui/candwnd_render.cpp`、`ui/candwnd_core.cpp`、`ui/candwnd_interact.cpp`、`common/settings.h/.cpp`、设置窗入口；**禁碰 `common/proto.h`、`ime/*`、`ui/engine/*`**。执行步骤与门禁见 `docs/impl-handoff-2026-11-03.md`。
