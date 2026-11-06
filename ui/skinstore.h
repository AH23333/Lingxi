// skinstore.h - 在线皮肤：目录扫描 + 运行时注册表 + URL 导入管线
//
// 线程模型（与 cellimport 同裁决）：扫描/导入只碰磁盘与内存副本，可在后台
// 线程调用；运行时注册表互斥保护，UiReloadSkins() 由 UI 主线程在启动与
// 导入/删除后调用，渲染侧经 UiCurrentSkin 读时解析（下次 Show/Paint 生效，
// 无需广播）。
#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "uistyle.h"            // SkinDef / UiSkin（内置肤回退）
#include "../common/settings.h" // Settings.skinId（在线肤 id 寻址）

namespace lingxi {

// 皮肤包目录（与 CellPackDirW 同约定：%APPDATA%\LingxiIme\skins；UI 侧创建，
// 扫描侧绝不创建）。
std::wstring SkinDirW();

struct DlSkin
{
    std::wstring id;
    std::wstring name;
    SkinDef def;
};

// 扫描目录：验证失败的件跳过（skipped 回填件数，可空）；按 (id,文件名) 升序，
// 重复 id 先到者胜；有效件上限 kMaxDlSkins。返回有效件数。
size_t ScanSkinDir(const std::wstring& dir, std::vector<DlSkin>* out,
                   size_t* skipped = nullptr);

// ---- 运行时注册表（互斥保护；magic static 不析构，退出竞态同 CandWindow 裁决）----
void UiReloadSkins();                          // 全量重扫 SkinDirW 并换入
size_t UiDlSkinCount();
bool UiDlSkinAt(size_t i, DlSkin* out);        // 越界 false
bool UiDlSkinById(const std::wstring& id, DlSkin* out); // 未命中 false

// 渲染单入口解析：skinId 非空→查注册表，命中→下载肤；未命中（已删/未扫到）
// → 回退内置 UiSkin(skinIndex)（越界再回退 0，SK4 语义不变）。
SkinDef UiCurrentSkin(const Settings& s);
// 解析后显示名（帮助窗快照）：命中→下载肤名；未命中→"<id>（已删除，回退内置）"；
// 空 skinId→内置名（kSkinNames）。
std::wstring UiCurrentSkinName(const Settings& s);

struct SkinImportResult
{
    bool ok = false;
    std::wstring message; // 人读结果或拒绝原因
    std::wstring id;      // 成功时有效
};

// 在线导入（管线与 ImportCellPackFromUrl 同型）：
//   URL 形闸（skinformat_logic.h：https/.lxskin/无 userinfo）→ 下载到 %TEMP%
//   staging（≤256KB）→ 完整性锚（显式 64-hex，或留空走同 origin 侧车
//   <url>.sha256；两者皆无=拒绝安装，fail-closed）→ 解析验证（魔数/必填/
//   色域/亮度/id）→ 规范化重写原子落盘（tmp+rename，同 id 直接覆盖——新件
//   全验证过后才触旧件）→ 独立复核（重读再解析逐项比对，不一致删新件报
//   "导入未确认"）。域名白名单由下载层二次把关（updater::DownloadToFile）。
// downloader 默认空 = 实网；测试注入假实现（零网络）。任何失败 staging 两件全清。
typedef bool (*SkinDownloadFn)(const std::wstring& url, const std::wstring& destPath,
                               size_t maxBytes, std::wstring* err);
SkinImportResult ImportSkinFromUrl(const std::wstring& url,
                                   const std::wstring& sha256HexOrEmpty,
                                   SkinDownloadFn downloader = nullptr);

// 删除已装皮肤。settings 正引用它时不需额外处理——UiCurrentSkin 自动回退内置。
bool DeleteSkin(const std::wstring& id, std::wstring* err);

} // namespace lingxi
