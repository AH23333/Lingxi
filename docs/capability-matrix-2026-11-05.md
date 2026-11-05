# LingxiIme 能力对照台账（2026-11-05，代码与断言实读为唯一源）

> 本文档取代 `gap-ledger-2026-11-05.md`（旧台账三次证伪，根因是从旧文档摘结论）。
> 规则：**凡"已有"必须带断言名或文件:行号作证；凡"真缺"以代码扫描无实现为准**。
> 断言计数口径：engine_test Release 单实例 PASS=519 FAIL=0（2026-11-05 17:53 读数）。

## 一、已有能力（带证据）

### 核心输入
| 能力 | 证据 |
|---|---|
| 全拼逐词+整句候选 | 断言 `sentence top is contextually correct`（14+ 句）、`pinyin: jintianqi -> 今*` |
| 整句开关 SentenceMode（缺省开） | 断言 `sentenceMode: *`×4；settings.cpp `SentenceMode` Load/Save/Diff |
| 简拼/首字母 | 断言 `learn: abbreviated 'mm' -> '喵喵'`、`P15 zm/zn/mm` 系列 |
| 混拼残段（尾部 PARTIAL 幸存） | 断言 `pinyin: jintianq (partial tail) non-empty`、`R4 rescue: partial-syllable typo` |
| 双拼 9 方案+方案切换 | 断言 `pinyin: shuangpin jbtm/vjlt`、`scheme1/2 end-to-end == quanpin ling` |
| 模糊音 8 组位掩码 | 断言 `fuzzy group gating`×6（sh/s、zh/z、nasal、l/r、n/l、f/h）；settings.h:31 fuzzyGroups |
| 智能纠错（编辑距离≤1 救援趟+静态表） | 断言 `R4 generalized typo` 系列、`typoCorrection: *`×4；pinyin.cpp BuildTypoIndex |
| 纠错学习键归一（typo 键不落词典） | 断言 `typo learn: *`×4；pinyin.cpp _rescueKeys |
| 分词符 '（音节边界） | 断言 `separator: xi'an` 系列×5 |
| 零声母 i/u 简写 | 断言 `zero-initial: i/u shorthand yields cands (B3)` |
| 组合内光标编辑（←/→/插入/退格） | 断言 `M5 caret: *`×3；ime/composition.cpp 距尾偏移模型 |
| 逐词上屏 cover 语义 | 断言 `cover: nihao/jintianqi`×4 |

### 学习与词典
| 能力 | 证据 |
|---|---|
| 选词学习+持久化 | 断言 `learn: after feedback '脏' promoted`、`persisted across reload` |
| 词频衰减（半衰期，manual 豁免） | 断言 `B5-1..4`；pinyin.cpp UserPickScore |
| 每键 8 上限 manual 不淘汰 | 断言 `EX1/EX2`；pinyin.cpp Learn 注释"审查确证缺陷①" |
| 用户词典 20000 键上限+批量淘汰 | 断言 `L10 exactly-20000/20001st/all-manual`×3 |
| 用户词管理（增删改合并） | 断言 `UD-C1..C6`、`MG3 ud line parse` |
| 无痕模式 | 断言 `incognito: *`×4 |
| 细胞词库（.lxcell 导入/启停/配额隔离） | 断言 `CELLa-0..12`/`CELLb-1..5`/`CELLc-1..6`/`CELLd`；ui/cellimport.cpp |
| 自定义短语（触发+导入导出+冲突三分支） | 断言 `R2 phrase`×5、`PH1/PH2/PH3` |
| 屏蔽词 | 断言 `R6 blocked word filtered`、`blocked hot-unload restores` |

### 特殊模式
| 能力 | 证据 |
|---|---|
| v 模式计算器（含大写金额） | 断言 `calc`×8、`calc tier1-6 17-digit rejected`、递归深度闸 |
| u 模式拆字 | 断言 `uradical: *`×9（含 un 接管、双拼门） |
| 日期时间直出（rq/sj/xq/rqsj/mt/ht/yt/nl） | 断言 `R3 datetime trigger`、`P5d`×9 |
| 符号搜索（12 类+AND+精确优先） | 断言 `symsearch`×12；ui/symsearch.cpp |
| emoji 跟随 | 断言 `R5 emoji`×4 |
| 简繁转换（一对一表 4012 行+门控） | 断言 `pinyin: traditional diannao -> 電腦` |
| 英文补全（纯 ASCII 上下文） | 断言 `en prefix predict`、`chain: *`×6 |
| LLM 重排（可选，默认关） | 断言 `chain: cjk ctx keeps LLM first` 等；预测层 llmRerank 开关 |

### 系统与健壮性
| 能力 | 证据 |
|---|---|
| 每页候选数可配（1..9，缺省 9） | 断言 `PIN`×3、`CandPageSize`×3、`DG1..3` |
| 皮肤/透明度（钳制+α映射+回退） | 断言 `SK1..SK7` |
| 设置 TTL 缓存+seq 自愈 | 断言 `M5-TTL`×3、`N6`、`N11` |
| 原子写+刷盘（词典/剪贴板/usage/细胞） | 断言 `C4`×6、`ROB2/ROB3/ROB5` |
| 停机安全（不析构+.dtor.tmp+僵尸复查） | 断言 `C1a..C1d` |
| 命名管道帧/取消/超时 | 断言 `C2`×3、`C3`×3 |
| 毒词典/畸形输入防御 | 断言 `M7`×9 |
| 候选截断（码点+代理对） | 断言 `TR1..TR5` |
| 全角字母数字 | 断言 `FW1/FW2` |
| 统计窗（计数+常选词 TOP10） | 断言 `hotwords: *`×3；ui/usagestats.cpp |
| 自动更新（清单哈希锚+回滚+自验） | update_test PASS=188（台架）；common/update_apply.cpp |

### UI 面
设置页（含模糊音 8 组/纠错/整句/页宽/皮肤透明度勾选，settingsui.cpp:1179+）、符号面板、统计窗、用户词/细胞词管理窗（MG10a-c 槽位断言）。

## 二、真缺（代码扫描无实现）

| 缺项 | 判定依据 | 感知/成本 |
|---|---|---|
| 云词库/账号同步 | 全仓网络调用仅 update 链（GitHub releases 白名单），无同步协议 | 高感知/高成本 |
| 细胞词库在线下载 | cellimport 仅本地文件导入（CELLc 系列断言均本地路径） | 中/高 |
| 皮肤在线主题 | 仅内置皮肤（SK4 越界回退到 skin 0，无下载路径） | 中/高 |
| 语音/手写/截图输入 | 无对应模块（ui/ 下无相关文件） | 中/极高 |
| 跨进程/跨机剪贴板同步 | clipboardui 仅本机 | 低/中 |
| 笔画查字（u+笔画序） | **用户裁决不做（2026-11-05）** | — |
| per-monitor DPI | GetDpiForSystem（ui/render 单源），**真机 blocked** | 中/中 |
| AI 长句改写/续写 | 仅候选重排级 LLM，无成句生成 | 低/高 |

## 三、本台账生成纪律

- 断言清单：`tools/engine_test/engine_test.cpp` 全部 `Report(...)` 名（260 条命名断言，
  含循环放大后 519 计数）。
- 此后任何"差距/补齐"清单只从第二节"真缺"表出；新增能力落地后必须从"真缺"挪到
  "已有"并带断言名，否则视为未落地。
