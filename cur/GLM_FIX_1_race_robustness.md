# GLM_FIX_1 — 数据竞争修复与注销健壮化(质检代理实施声明)

> 按 cur/HANDOFF.md 第四节协议:动代码前先落盘声明。开发代理(豆包)若复活,请先读本文件再动手,避免撞车。
> 实施:iF 支代理(GLM)。全部改动走 **`qc/glm-fixes` 分支**,小步提交,每步构建验证后再下一步。

## 背景与现状

- 你的 `ce676de` 已合入用户词典功能;我在 QC-REPORT §三 指出的 **`_userDict` 三线程无锁竞争在提交版依然存在**(grep 证实 pinyin.cpp 零锁)。
- 你的 symbolpanel WIP(+49 行 + ui/symbolpanel.cpp/h,MSBuild 23:50 起僵尸空转)我**不会在原分支碰**:已快照到 **`wip/symbolpanel-doubao` 分支**(提交信息注明"构建未验证"),你复活后可在其上继续或 rebase。
- 僵尸的 3 个 MSBuild 节点(23:50 起 CPU≈1s)我将清掉——它们是构建节点,不是用户进程,不违反铁律。

## 我要改的文件与方案

### A. `ui/engine/pinyin.cpp`(+predictor.h 无需改)——修 P0/P1 数据竞争
1. 加 `#include <mutex>` 与成员 `mutable std::mutex _udMtx;`(保护 `_userDict`/`_userDirty` 及其落盘);
2. `LoadUserDict()` 整体持锁;
3. `SaveUserDict()` 拆为 `SaveUserDictLocked()`(调用方持锁)+ 公开版持锁包装;`Learn()` 持锁后调 Locked 版;
4. `Predict` 中 `_userDict.find(key)` 段:持锁拷出 `vector<UserPick>`,排序与拼接在锁外(读多写少,临界区 <µs 级,不碰热路径预算);
5. `UserDictPath()` 落盘改为**原子写**:写 `path + ".tmp"` 后 `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`,杜绝双写者撞车时的截断/半截文件。

### B. `ui/settingsui.cpp` —— 同一文件双写者的另一半
- `WriteFileBytes` 改为临时文件 + `MoveFileExW(REPLACE_EXISTING)` 原子替换(引擎侧 A5 同款语义);
- `PersistUdEntries` 检查写入失败,失败给 MessageBox 提示(修 P2 静默丢数据)。

### C. `ime/dllmain.cpp` + `tools/unregister.cmd` —— 注销假成功(幽灵 TIP 路径)
1. `UnregisterProfiles/UnregisterCategories/UnregisterServer` 返回 HRESULT/LSTATUS;
2. `DllUnregisterServer` 汇总,任一 CTF 注销失败 → 返回 `SELFREG_E_CLASS`(regsvr32 /u 将报错而非假 "Done");
   - 注意:`DllRegisterServer` 失败回滚路径调用 `DllUnregisterServer()` 处保持 best-effort,不因回滚失败改变原有返回值;
3. `unregister.cmd`:注销后查询 HKLM CLSID/TIP 键,仍存在则打印 WARNING(需管理员),并保留退出码语义。

### D. 小修(P2)
- `ime/service.cpp:120` 注释错别字"绝不肏"→"绝不碰";
- `ime/service.h` 删除全库无引用的死成员 `_fInPinyinEdit`;
- `docs/PROGRESS.md` "翻页待做"→"翻页已上线";`docs/PITFALLS.md` #20 补充"现行 AppendToFile 逐行开闭文件,该坑已随实现消除"的更正;
- `.gitignore` 增加 `cur/` 例外?——不,改为:cur/ 已被 96ec1fb 纳入跟踪(HANDOFF.md),我的 QC-REPORT.md 与本文件随修复分支一并提交,保持双代理交接留痕。

## 我不会碰的

- `ui/symbolpanel.*`、`ui/main.cpp`、`ui/pipeserver.*`、`common/proto.h`、`ime/pipeclient.cpp` 的 symbolpanel 相关改动(你 WIP 的领地,已快照分支);
- 任何用户进程(铁律 #1);`LingxiUI.exe` 不重启(仅构建改名让路);
- `HKCU\Software\LingxiIme`:待全部构建+测试通过后,按你 HANDOFF 第六节的既定语义("删除该键=全默认")删除,恢复出厂设置(当前全 0 是旧 bug 输出)。此步最后做,单独向用户汇报。

## 验证门

每步:`tools\build.cmd Debug` → `bin\enginetest\engine_test.exe`(勿带 LINGXI_LOG)→ 全部完成后 `bin\Debug\e2e.exe` 应 exit=0。分支合并回 main 由你或用户决定。
