# 实现交接要点（细胞词库 / 词频衰减 / 横竖排切换）

- 依据：三份设计稿 + 2026-11-03 用户裁决（各稿末尾"裁决"节为唯一权威）。
- 本稿由设计线产出，**全部结论基于静态阅读，未构建未运行**；行号是 2026-11-03 磁盘状态（HEAD `917b5a7` 之后并行会话持续在改 `tools/engine_test/engine_test.cpp`、`ui/symbolpanel.cpp`、`ui/symsearch.cpp` 等）——**动工前先用下列符号名重定位，不要盲信行号**。
- 通用门禁（每步跑）：MSBuild 直调 `/m:1`（禁 cmd /c，防 C1041）；Release x64 + Debug x64 全解决方案零错误零警告；engine_test 沙箱（临时 APPDATA + `LINGXI_SETTINGS_NS=test`）PASS ≥ 动工时最新基线、FAIL=0；有 `tools\regress.cmd` 则全量 ALL PASS；deep_test/e2e 只在无用户活跃窗口的静默环境串行跑（PITFALLS #92/#95）。每条改动独立 commit，说明改动原因。**禁止放宽既有行为断言或删除用例**（只允许按新格式扩展解析）。

## 0. 建议执行顺序：C → B → A

理由：C 纯 UI、零既有断言塌陷、可最先出货；B 只动引擎内部 + 文件格式加列，是 A 的地基（A 的导入适配层可复用 B 的列解析扩展）；A 面最大（新子系统 + 后台线程 + 新窗），放最后。三者互不依赖，拆开提交。

---

## 1. 任务 C：候选窗横/竖排切换（先行）

改动面锁定（裁决）：`ui/candwnd_render.cpp`、`ui/candwnd_core.cpp`、`ui/candwnd_interact.cpp`、`common/settings.h/.cpp`、设置窗入口。**禁碰 `common/proto.h`、`ime/*`、`ui/engine/*`。**

| # | 步骤 | 完成后立刻跑 |
|---|---|---|
| C1 | settings 新键 `UI.candOrientation`（0=竖排缺省 / 1=横排），非法值 clamp 回 0（照抄 candidateCount 的 1..9 clamp 先例 settings.cpp L187 附近） | Release+Debug 构建；engine_test 全量（应零塌陷） |
| C2 | `candwnd_core.cpp` LayoutAndPosition：横排高度 = pad×2+compH+itemH（无行数项）；宽度 = Σ(标签+词+注音+分隔)，cap=rcWork宽−2×gap；**放不下 → 本次渲染回退竖排**（裁决③，不做可见数回推） | 构建 + engine_test |
| C3 | `candwnd_render.cpp` OnPaint 横排分支：逐项左起绘制、单候选 8 字 cap 省略号、toast 分支（L198-L222 附近）横排下禁用；行高含注音两行的公式需真机校准 | 构建 + engine_test |
| C4 | `candwnd_interact.cpp` 四处命中测试（搜 `row = (pt.y - yOff) / itemH`，现 L184/L497/L617/L754）横排改列命中；悬停/单击/右键/滚轮翻页语义不变 | 构建 + engine_test |
| C5 | 设置窗加切换行（遵守 AGENTS.md：优先新文件/既有行工厂，settingsui.cpp 只减不增纪律拿不准就先问） | 构建 + regress.cmd |
| C6 | **真机（必须人工，但门槛低）**：横排截图 100/125/150% DPI、贴右缘翻转、贴下缘翻上、副屏负坐标、悬停与数字键一致性 | deep_test T12/T14 全过（预期零影响） |

**塌陷预期：无。** 缺省 0 = 现行为逐像素不变（裁决②：默认保持竖排，用户当前无法注销重登，不允许未被告知的外观变化）。候选窗在 **LingxiUI.exe** 内，真机验证只需重启 UI 进程、**不需要注销/重启系统**（区别于 TSF DLL）。

## 2. 任务 B：查询时词频衰减

改动面：`ui/engine/pinyin.cpp`（UserPick 结构、Learn、序列化、排序合并、淘汰）、`common/settings.h/.cpp`（`Decay.halfLifeDays`）、`tools/engine_test`（新用例 + 格式断言修订）。不动协议、不动 `ime/*`。

| # | 步骤 | 完成后立刻跑 |
|---|---|---|
| B1 | `UserPick` 加 `int64_t lastUsed`；LoadUserDict 读可选第 5 列（非法/缺失=now，照第 4 列 manual 先例）；序列化：仅当存在非缺省 last_used 才写第 5 列，且**第 4 列补占位 `0`** | 构建；engine_test ——**预期此步起 §2.1 的格式断言塌，同 commit 内修订** |
| B2 | Learn 写 `lastUsed=now`；`score()` 纯函数（注入 now，`count × 2^(−age/H)`，age 钳 ≥0，manual 直通 count） | engine_test |
| B3 | 候选合并（原 L499-L505）与 EvictUserDictKeysLocked 的 keyTotal（原 L1320-L1367）**同源切 score**（裁决③，禁止两套口径） | engine_test + regress.cmd |
| B4 | settings `Decay.halfLifeDays` clamp [7,365]；`Decay.enabled` 开关（0=score=count） | engine_test |
| B5 | 新用例：半衰期边界（age=0/H/2H）、时钟回拨、manual 豁免、"高count极旧键 vs 低count昨天键"淘汰序、3/4 列老文件逐字节兼容（缺省 now → 零衰减 → 既有用例即回归基线） | engine_test 全量 |
| B6 | 真机：跨会话 last_used 生效观察（观察项，可脚本化的先脚本化）；deep_test 静默环境串行 | deep_test 全过 |

### 2.1 会塌的既有断言（B 的完整清单，符号名定位）

| 位置（现读锚点） | 断言 | 为什么会塌 | 处置 |
|---|---|---|---|
| engine_test "脏\tzang\t2" 精确行文本（搜 `zang`，现 L1396 附近） | 落盘内容逐字比对 | Learn 后该键带 last_used → 行变 `脏\tzang\t2\t0\t<ts>` 5 列 | 断言改"前 3 列逐字 + 第 5 列存在且为数字"；**不许改成只查行数** |
| L10 A1 `exactly-20000: no eviction, lines valid`（现 L2192） | 逐行格式有效性 | 学习键行变 5 列 | 解析器扩到 5 列，行为断言原样保留 |
| L10 A2 `20001st key: evict-1000, manual+high-count survive`（现 L2219） | 淘汰集合 | 测试内即时序 age≈0 → score=count → 集合不变；**若断言含行文本格式则塌** | 复核该断言的解析面，扩列 |
| L10 B `all-manual 20001 ... 每行带第 4列"1"`（现 L2223/L2250） | 第 4 列存在 | MergeUserDict 若也写 last_used → 第 4 列仍在，断言可活；若实现选择"manual 不写第 5 列"则完全不塌 | 实现建议：manual 词条不写 last_used（反正豁免），此断言零改动 |
| M7 `userdict load does not rewrite file`（现 L2650） | 只读加载不改文件 | 与 B 无关 | 预期不塌，跑过为证 |
| 原子性工装（Learn+Flush 后读快照，现 L2432-L2456） | 行数/收敛 | 列数变化不影响行数断言 | 复核即可 |
| PIN `kPageSize==9`、candidateCount 默认值（现 L2360-L2364） | 协议/设置常量 | B 不触碰 | 不塌 |
| deep_test T12（翻页+第二页数字，现 L1167-L1198）/ T14（部分提交，现 L1227-L1284） | 键盘路径 | 测试时间尺度 age≈0，排序恒等 | 预期不塌；B3 合入后**必须实跑一次钉死**（静态推断，非验证） |

## 3. 任务 A：细胞词库

改动面：`ui/engine/pinyin.cpp`（CellPack、并入 rank 段、启停）、`common/settings.h/.cpp`（`Cell.disabled`）、新文件词库管理窗（AGENTS.md：新功能新文件）、`ui/main.cpp`（g_hooks 装配 + 后台导入完成消息）、导入适配层。不动协议常量。

| # | 步骤 | 完成后立刻跑 |
|---|---|---|
| A1 | `.lxcell` 解析器（纯函数、可单测）：头校验/UTF-8+BOM/跳过行计数/单包 200k、合计 500k 限额 | engine_test 新用例（格式边界全套，设计稿 §7.1-2） |
| A2 | 导入=后台线程**只解析+写临时文件**（tmp+rename 落 `%APPDATA%\LingxiIme\cell\`）；完成后 PostMessage 回主线程做引擎合并——**`_entries`/五套索引无锁，严禁工作线程触碰**（裁决⑤的实现红线） | 构建；engine_test |
| A3 | 引擎并入：包词条追加 `_entries` 尾部、每包连续 rank 段；停用=重建时跳过该段；**细胞词条绝不进 `_userDict`**（裁决①独立配额） | engine_test —— 重点看 L10 全套 |
| A4 | 启停走 settings `Cell.disabled` + ReloadUserDict 同款 Flush→重建路径；导入进度 UI（后台+进度，裁决⑤） | regress.cmd |
| A5 | 词库管理窗（安装/列表/启停/删除/导入结果"成功N跳过M+行号"，禁静默丢弃） | 构建 + regress.cmd |
| A6 | 导入后独立复核：重读落盘文件头+行数与内存计数比对，一致才报成功（9e3f5dc 教训外推） | engine_test |
| A7 | 真机：大包导入时打字延迟主观测（设计稿 §6 的 300ms 解析预估是纸面值，**必须 bench 实测**）；启停即时生效 | deep_test 静默串行 |

### 3.1 会塌的既有断言（A 的完整清单）

| 位置 | 断言 | 塌陷条件 | 处置 |
|---|---|---|---|
| L10 全套（现 L2091-L2252） | 20000 封顶/淘汰集合/第 4 列 | **仅当细胞词条被错误并入 `_userDict`** | 裁决①已堵死此路；A3 合入后全量跑 L10 为证 |
| ReloadUserDict 既有用例（现 L1606/L1644/L1753/L1764） | 重载后行为 | cell 目录不存在时必须严格 no-op | 新用例补"无 cell 目录 = 与现在逐字节一致" |
| 页边界用例（现 L583、L787-L806）+ deep_test T12/T14 | kPageSize=9 语义、"ceshi 11 条>9" | 测试沙箱若自动启用词库包 → 候选集合变化假红 | 测试环境**不装任何包**；`LINGXI_SETTINGS_NS=test` 天然隔离 cell 目录则免改，否则显式禁用 |
| 启动加载耗时（regress 若有时间预算） | 30 万行基线 | 500k 行包全启用会拉长启动 | 限额已封顶；bench 出数再定 |

## 4. .scel 转换工具研判（裁决 A② 的落地建议）

**结论：不需要自研，且不建议自研。** 检索确认（2026-11-03，外部信息，未在本机实测）：

- 社区逆推已完全成熟：**深蓝词库转换（imewlconverter，studyzy/imewlconverter）**支持搜狗 .scel 与 20+ 种输入法词库互转，是 rime 生态（如 cloverpinyin）的标准前置工具；另有 Python 库 `ciku` 等多个开源实现。自研解析器 = 重复造轮 + 独自承接格式变体风险，负收益。
- **建议路线**：产品侧只把工程量花在"导入适配层"——让 `.lxcell` 导入直接接受深蓝转换后的纯文本产物（词+拼音码行格式；**具体列式需在实现期用真实产物核对，本稿未验证**），预计 ≤0.5 人日，可并入 A1。
- 样本来源：搜狗官网细胞词库下载页（.scel 公开可下载）；校验正确性 = 自动化三条（GBK 解码零错误、音节全部命中本引擎音节表、行数=头声明数）+ 人工抽样对照搜狗官网词条预览。
- 若将来要"产品内一键选 .scel"：届时移植社区解析器为独立控制台小工具（1~2 人日），注意 imewlconverter 为 copyleft 许可——**只能外部使用，不可链接进产品**；`ciku` 许可需实现期核对。
- 风险声明：以上生态信息来自本轮网络检索（见下方 Sources），非本机验证；动工前实现方应下载 2~3 个真实 .scel 跑一遍深蓝产物，确认格式假设。

## 5. 收口清单（实现方逐条打勾）

- [ ] C1-C6、B1-B6、A1-A7 按序完成，每步门禁读数记录在 commit 信息
- [ ] §2.1 / §3.1 塌陷清单逐条复核：预期塌的已修订、预期不塌的有跑过的证据
- [ ] 无任何断言被放宽/删除；无 `proto.h`/`ime/*` 触碰（C 第一期禁区内）
- [ ] deep_test T12/T14 在 B3、A3 合入后各实跑一次（静默环境）
- [ ] 真机项（C6、B6、A7）单独列给用户，不与台架读数混报
- [ ] .scel：仅做导入适配层；自研解析器未动工（除非用户另行拍板）

Sources:
- [搜狗词库scel格式转txt文本 - CSDN](https://blog.csdn.net/Beyond_F4/article/details/80372514)
- [深蓝词库转换工具介绍 - 中华网软件](https://soft.china.com/details/82/2829019.html)
- [rime-cloverpinyin（使用深蓝转换 scel 的实例）- Gitee](https://gitee.com/fkxxyz/rime-cloverpinyin)
- [ciku - PyPI 词库解析库](https://pypi.org/project/ciku/0.3.2/)
- [imewlconverter Wiki: scel 格式说明](https://github-wiki-see.page/m/studyzy/imewlconverter/wiki/Sougou_Pinyin)
