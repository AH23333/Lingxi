# LingxiIme 系统集面只读审查（2026-11-04）

> 范围：TSF 注册面 / 卸载还原 / 提权面 / 自动更新链 / IPC 与隐私 / 本机残留现状。
> 方法：纯静态阅读 + 只读系统查询（reg query / Get-Item / Get-Process）。未改任何文件、未跑安装/卸载/测试、未写注册表。
> 标注约定：**[确证]** = 读代码/读系统可直接断定；**[需实测]** = 需运行或真机才能定论。

---

## 结论先行

整体评价：**防御设计显著高于同类自研输入法平均水平**——注册失败自清理、注销失败不装成功、下载白名单逐跳复核、zip-slip 预校验、替换两阶段全回滚、自验三项独立复核、卸载零残留自检，均为正确形态。

未发现 P0（立即可利用/数据丢失级）。最该处理的 3 条：

1. **P1 [确证]** 卸载零残留自检漏查 `HKCU\SOFTWARE\Classes\CLSID\{tip}`（per-user CLSID 回退注册的落点）——非管理员安装 + DLL 先被删的路径下，脚本报"零残留"实际留有幽灵 CLSID。`tools/uninstall.ps1:51-54`（清理）与 `:113-125`（自检清单）。
2. **P1 [确证]** 提权 `--update-apply` 信任锚在 staging 目录本身：`VerifyInstalledHashes` 比对的是"落盘件 vs staging 包内件"（`common/update_apply.cpp:281-300`），不是网络下发的 SHA256SUMS。staging 位于用户可写的 `bin\work\staging\`，下载期校验与提权期替换之间存在 TOCTOU 窗口，同用户进程可在窗口内换包后由提权进程完成机器级注册。
3. **P2 [确证]** HKLM CLSID/TIP 注册的 InprocServer32 指向**用户可写路径**（源码树 `bin\Debug\ImeCore.dll`，本机实测注册表值如此）——机器级注册 + 用户级可写二进制，任何同用户进程可静默换 DLL 并被所有新宿主加载。属开发树部署的固有风险，需在文档中明示或提供 Program Files 安装形态。

---

## P0

无。

---

## P1

### P1-1 卸载自检漏查 per-user CLSID（误报"零残留"）[确证]

- **位置**：`tools/uninstall.ps1:50-55`（belt-and-braces 清理）、`:113-125`（9 项零残留校验）；对照 `ime/dllmain.cpp:122-168`（`RegisterServer` 无管理员时回退写 `HKCU\Software\Classes\CLSID\{tip}`）。
- **当前行为**：正常路径 `regsvr32 /u` 会调 `DllUnregisterServer`，后者 `RegDeleteTreeW(HKCU, ...)`（dllmain.cpp:305）能删掉 per-user CLSID。但若 DLL 文件已不存在或 regsvr32 失败（ps1 第 44/47 行 `$null = ... 2>$null` 吞掉退出码），兜底清理只覆盖 `HKLM\SOFTWARE\Classes\CLSID`、`HKLM WOW6432Node\CLSID`、`HKLM\CTF\TIP`、`HKCU\CTF\TIP` 四处——**不含 `HKCU\SOFTWARE\Classes\CLSID\{tip}`，也不含 32 位对应的 `HKCU\SOFTWARE\Classes\WOW6432Node\CLSID\{tip}`**；9 项自检同样不含这两键。
- **最坏后果**：用户先删源码树再跑卸载 → 脚本输出"SUCCESS: zero residue"，实际 HKCU 下留有指向已删 DLL 的 CLSID → 输入法列表出现幽灵条目，宿主尝试 CoCreate 失败（无害但脏），且**自检绿给后续排查误导**（项目纪律明确禁止"静默跳过冒充绿"）。
- **建议**（只建议不改）：清理段与自检段各补 `HKCU\SOFTWARE\Classes\CLSID\{tip}` 与 `HKCU\SOFTWARE\Classes\WOW6432Node\CLSID\{tip}` 两键；regsvr32 的退出码不要吞，失败时在最终报告中单列"regsvr32 /u 未执行成功"。

### P1-2 更新链信任锚在提权前断链（staging TOCTOU）[确证]

- **位置**：`ui/main.cpp:487-511`（提权侧只校验路径合法性 `ValidateStagingPath`）；`common/update_apply.cpp:281-300`（`VerifyInstalledHashes` 比对 dst vs **staging**）；`docs/update-design-2026-11-03.md` T10。
- **当前行为**：下载期（非提权）对 zip 做 SHA256SUMS 比对，解压到 `bin\work\staging\<版本>\`；随后一次性 UAC 拉起 `--update-apply`，提权进程**只验 staging 路径在 workRoot 之下且名为合法版本串**，不重新校验 staging 内容与网络下发 SUMS 的绑定；自验第 3 项的哈希比对两侧都以 staging 为准。
- **最坏后果**：下载校验完成到 UAC 确认之间，staging 可被同用户（非提权）进程改写；提权进程随即把被换的 DLL 复制到 binRoot 并以管理员身份 `regsvr32` 写 HKLM——机器级注册一个被篡改件。利用前置条件是"同用户已有恶意代码执行"，此时 binRoot 本身也可被直接改写，故此条定为 P1 偏下而非 P0；但它使"UAC 确认"这一安全 UX 失去意义（用户以为确认的是已校验件）。
- **建议**：下载期把 SUMS 的校验结果（或 SUMS 文件本身）以提权进程可复核的形式锚定——最简方案：解压完成后由非提权侧把"staging 全文件 SHA256 清单"写入 `apply-request` 并随 UAC 命令行/受保护位置传入，提权侧先按清单复核 staging 再替换；复核失败直接拒绝。

---

## P2

### P2-1 机器级注册指向用户可写二进制 [确证]

- **位置**：本机注册表实测 `HKLM\SOFTWARE\Classes\CLSID\{tip}\InprocServer32 = "d:\Visual Studio Code\Something\LingxiIme\bin\Debug\ImeCore.dll"`（WOW6432Node 同型指向 `bin\Debug32`）；写键代码 `ime/dllmain.cpp:141-156`。
- **当前行为**：开发树直注册，DLL 路径在用户目录下。
- **最坏后果**：同用户任意进程可替换该 DLL，之后**每个新建宿主进程**（含以管理员身份运行、加载 TIP 的程序——TIP 在宿主进程内加载）都执行被换代码。机器级注册放大了用户目录二进制的暴露面。
- **建议**：文档明示"开发形态仅限本机调试"；发布形态提供安装器复制到 `Program Files`（管理员拥有写权）后再注册。属架构建议，非代码缺陷。

### P2-2 卸载提权在异账户下清错用户的数据 [确证]

- **位置**：`tools/uninstall.cmd:8-13`（UAC 重拉）+ `uninstall.ps1` 全部 HKCU/APPDATA 操作（语言列表 L24-41、SortOrder L62-74、Run 键 L57-59、-PurgeUserData L83-88）。
- **当前行为**：若当前账户非管理员、UAC 输入的是**另一个管理员账户**，则 HKCU 系清理与 `%APPDATA%` 清除全部作用于管理员账户，安装者账户的 Run 键/语言列表/用户数据原样残留，自检却全绿（自检读的也是管理员的 HKCU）。
- **最坏后果**：误删管理员账户同名键（低概率但非零：管理员恰好也装过则清的是他的配置）；卸载报告失真。
- **建议**：脚本开头比对 `whoami` 与推断的安装者（如 Run 键值里的路径属主），异账户时给出黄色警告；文档注明"须以安装者本人身份提权"。

### P2-3 下载直写目标文件、无签名验证 [确证，设计已声明]

- **位置**：`common/update_fetch.cpp:236-287`（下载直写 destPath，失败删除残件 ✓）；`docs/update-design-2026-11-03.md` T3（如实声明 release 页整体失守则无法再防）。
- **当前行为**：信任根 = GitHub release 页 + HTTPS；无 Authenticode/离线公钥签名验证。T3 已如实声明此边界。
- **建议**：维持声明即可；若未来收紧，最低成本是 WinVerifyTrust 检查 Authenticode（前提是有签名证书预算）。

### P2-4 卸载时宿主仍加载 DLL 的残留语义未向用户讲清 [确证]

- **位置**：`uninstall.ps1:21-22` 只停 LingxiUI，不停/不能停加载了 ImeCore.dll 的宿主（explorer 等）；脚本末尾 L131 提示"源码树可删除"。
- **当前行为**：注册键已清、语言列表已还原，但磁盘 DLL 仍被宿主映射：用户此时删树会遇文件占用，留下无注册的孤儿 DLL（无害但脏）；宿主进程内输入法"还能用"直到注销——用户可能误判卸载失败。
- **最坏后果**：无安全后果；体验/认知失真。
- **建议**：卸载末尾明示"已加载本输入法的程序需注销重登后才完全卸载；删不掉的 DLL 文件注销后再删"。（脚本已有类似措辞则忽略。）

---

## P3

- **P3-1 [确证] 诊断日志会落用户击键拼音**：`ime/composition.cpp:440` `LX_LOG(L"ime", L"组合开始 pinyin=%ls", ...)`。仅 `LINGXI_LOG=1` 环境变量显式开启（`common/log.h:24-29`），日志在 `%TEMP%\lingxi-ime.log`（ACL 继承用户私有目录，1MB 轮转 ✓）。上屏内容只记长度不记文本（service.cpp:643/648 ✓）。评估：opt-in 诊断可接受，建议文档提示"诊断日志含击键拼音，分享日志前知悉"。
- **P3-2 [确证] 命名管道 DACL 正确**：`common/pipe_util.cpp:9-50`，SDDL `D:(A;;GA;;;<当前用户SID>)`，同机他账户不可连；token 读取失败回退默认 DACL（注释自证，属窄回退）。✓
- **P3-3 [确证] 卸载脚本无误删他者键的风险面**：SortOrder 清理逐项按 CLSID 匹配（ps1:62-74），`Remove-Item -Recurse` 仅用于自身 `AssemblyItem` 子项与自身 APPDATA 目录；四个 `reg delete` 均为精确键。✓
- **P3-4 [确证] 注册表 Run 值含字面 `tools\..`**：实测 `LingxiIme = "d:\...\tools\..\bin\Debug\LingxiUI.exe"`（register.cmd:43 未规范化 `%~dp0..`）。Windows 可解析，纯外观问题。
- **P3-5 [需实测] 双位注册共享 HKLM profile，IconFile 后写者胜**：实测两语言 profile（0409/0804）的 `IconFile` 均为 `bin\Debug32\ImeCore.dll`（32 位后注册覆盖了 64 位写的值）。图标加载不受影响（同 CLSID 双位同名 DLL），但 profile 描述/图标的路径语义是"最后注册的一侧"。建议知晓即可。

---

## 注册面全清单（谁写、谁删、谁验）

| 键/值 | 写入者 | 删除者 | 卸载自检 |
|---|---|---|---|
| `HKLM\SOFTWARE\Classes\CLSID\{tip}`(+InprocServer32) | regsvr32→`RegisterServer`（dllmain.cpp:131-166），无管理员回退 HKCU | `DllUnregisterServer` RegDeleteTreeW（HKLM+HKCU，dllmain.cpp:304）+ ps1 兜底 reg delete | ✅ HKLM 侧；❌ HKCU 侧（P1-1） |
| `HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{tip}` | SysWOW64 regsvr32（register.cmd:28） | regsvr32 /u + ps1 兜底 | ✅ HKLM 侧；❌ HKCU WOW64 侧 |
| `HKLM\SOFTWARE\Microsoft\CTF\TIP\{tip}` 全树（Enable/Category/LanguageProfile 0409+0804） | `RegisterProfiles`/`RegisterCategories`（dllmain.cpp:171-245） | `UnregisterProfiles`/`UnregisterCategories`（无管理员必失败，错误上传播 dllmain.cpp:247-295 ✓）+ ps1 兜底 | ✅ |
| `HKCU\Software\Microsoft\CTF\TIP\{tip}`（per-user 影子） | CTF 自维护 | ps1 兜底 reg delete | ✅ |
| `HKCU\...\SortOrder\AssemblyItem` 项 | CTF 自维护 | ps1 按 CLSID 匹配逐项删 | ✅ |
| 语言列表 InputMethodTips | 用户/系统 | ps1 `Set-WinUserLanguageList` 剔除，保底回退默认键盘（ps1:29-34 ✓） | ✅ |
| `HKCU\...\Run\LingxiIme` | register.cmd:43 | ps1 Remove-ItemProperty | ✅ |
| `HKCU\Software\LingxiIme`（设置） | 运行时 SaveSettings | 仅 `-PurgeUserData` | ✅（默认期望 KEPT，口径正确） |
| `%APPDATA%\LingxiIme`（词典/剪贴板/统计/备份） | 运行时 | 仅 `-PurgeUserData` | ✅（同上） |

---

## 本机残留现状（只读核对，2026-11-04）

- **注册态**：HKLM x64 + WOW6432Node CLSID 均在，指 `bin\Debug` / `bin\Debug32`；HKLM TIP 树完整（Enable=1，双 profile）；HKCU CLSID/TIP 无影子；SortOrder 无本 TIP 项；Run 键在。**当前是"完整已注册"状态，无半注册残留。**
- **DLL 与宿主**：磁盘 x64 DLL（11-03 13:50:05）与唯一加载宿主 `Trae CN`(PID 17600) 映射的镜像**同时间戳**——当前无"磁盘新、进程旧"失真；但此状态只保证此刻，任何后续重建后未注销重登即回到旧镜像态（项目既有认知）。
- **更新残留**：`bin\` 下无 `.prev-*` 快照；`bin\work\` 只有构建中间目录，无 staging/download 残留——更新链未在真机执行过替换，无半替换态。
- **用户数据**：`%APPDATA%\LingxiIme` 含 userdict.txt / clipboard.dat / usage.txt / backups，健在（与卸载默认保留口径一致）。
- **如需安全还原（不执行，仅步骤）**：管理员身份跑 `tools\uninstall.cmd` → 核对输出 9 项全 OK → 注销重登 → 再删源码树；若曾以非管理员装过，补删 `HKCU\SOFTWARE\Classes\CLSID\{ae967759-f054-4e02-ac5d-c172f1e9b99b}`（及 WOW6432Node 对应键）后再信自检结论。

---

## 附：已排除（读码确认无问题，勿重复排查）

- 注册半失败自清理：`DllRegisterServer` 三步任一失败即调 `DllUnregisterServer` 清半注册态（dllmain.cpp:312-325）✓
- 注销假成功防线：CTF 注销失败返回 SELFREG_E_CLASS 让 regsvr32 显式报错（dllmain.cpp:327-339）✓（ps1 吞退出码是其自身问题，见 P1-1）
- 历史误注册类别 `TIPCAP_INPUTMODECOMPARTMENT` 在注册/注销两侧均幂等清理（dllmain.cpp:235-237/286-288）✓
- 下载：HTTPS+精确域名白名单+逐跳重定向复核+Content-Length 预检+超限即删残件（update_fetch.cpp:168-289；update.cpp:382-439）✓
- 替换：rename-aside→复制两阶段、任一失败全回滚、`.prev-<ts>` 撞名加序号、自验三项不过整体回滚、回滚自身失败必上报不吞（update_apply.cpp:390-457/475-508）✓
- 提权侧入口窄化：只接受 `bin\work\staging\<合法版本>`，结果写文件供 UI 独立读（main.cpp:487-511）✓
- 管道取消后等待落定、整帧持锁、停机不析构等历史高危区未被本轮系统集成改动波及 ✓
