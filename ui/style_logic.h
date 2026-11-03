#pragma once
//
// ui/style_logic.h — 皮肤/透明度链路纯逻辑层（批15）。
// 链条：设置页下拉（OpacityFromComboSel/OpacityComboIndex）→ 持久化
// （common/settings 注册表 CandidateOpacity/SkinIndex，越界拒绝→缺省）→
// 候选窗生效（ClampOpacity 双保险 + OpacityToAlpha → SetLayeredWindowAttributes）。
// 判定口径集中本头，engine_test SK 块直链单测；分层渲染实际观感
// （半透明透出桌面、皮肤色板）blocked 待真机。
//
#include <cstdint>
#include <windows.h> // MulDiv

namespace lingxi {

constexpr uint32_t kOpacityMin = 70;
constexpr uint32_t kOpacityMax = 100;

// 不透明度 % 钳制到 [70,100]（候选窗 ApplyOpacity 与 Shift+滚轮共用口径；
// LoadSettings 已拒绝越界持久值，此处为渲染端双保险）
inline uint32_t ClampOpacity(int op)
{
    if (op < (int)kOpacityMin) return kOpacityMin;
    if (op > (int)kOpacityMax) return kOpacityMax;
    return (uint32_t)op;
}

// % → LWA_ALPHA 字节值（MulDiv 四舍五入：70→179, 80→204, 90→230, 100→255）
inline int OpacityToAlpha(uint32_t op)
{
    return MulDiv((int)op, 255, 100);
}

// 设置页"不透明度"下拉：显示序 {不透明,90%,80%,70%} ↔ 数值 {100,90,80,70}。
// 审查缺陷⑤：combo 选项集 {100,90,80,70} 窄于合法域 [70,100]——越界选择
//（无选择/控件未就绪）保留 cur，不再强制回缺省 100（静默重置）。
inline uint32_t OpacityFromComboSel(int sel, uint32_t cur)
{
    static const uint32_t kOps[] = { 100, 90, 80, 70 };
    return (sel >= 0 && sel < 4) ? kOps[sel] : cur;
}

// 数值 → 下拉项；非选项值（如 85）回显 CB_ERR 无选择——配合保存侧
// 保留语义，"打开设置页保存"不会把 85 静默改成 100。
inline int OpacityComboIndex(uint32_t op)
{
    static const uint32_t kOps[] = { 100, 90, 80, 70 };
    for (int i = 0; i < 4; i++) if (kOps[i] == op) return i;
    return CB_ERR;
}

} // namespace lingxi
