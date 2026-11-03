#pragma once
//
// ui/combo_logic.h — 设置页"combo 选项集窄于注册表合法域"统一口径
//（审查缺陷⑤）。旧口径：域内非选项值回显缺省选项 → 任意保存静默重置。
// 新口径：回显侧非选项值置 CB_ERR（无选择），保存侧 GetCurSel<0 保留
// 原值——"打开设置页并保存"不得改动用户经外部途径设置的域内非选项值。
// 判定口径集中本头，engine_test CS 块直链单测；真实控件交互 blocked 待真机。
//
#include <cstdint>
#include <windows.h> // CB_ERR

namespace lingxi {

// 保存侧：combo 选择 → 选项值；sel<0（无选择/控件未就绪）→ 保留 cur
inline uint32_t ComboSelToValue(int sel, const uint32_t* opts, int optCount, uint32_t cur)
{
    return (sel >= 0 && sel < optCount) ? opts[sel] : cur;
}

// 回显侧：值 → combo 下标；不在选项集 → CB_ERR（无选择，不误导回缺省项）
inline int ComboValueToSel(uint32_t v, const uint32_t* opts, int optCount)
{
    for (int i = 0; i < optCount; i++)
        if (opts[i] == v) return i;
    return CB_ERR;
}

// 每页候选数：combo {3,5,6,9} 窄于合法域 1..9（settings.cpp CandPageSize）
constexpr uint32_t kPgSizeOpts[] = { 3, 5, 6, 9 };
inline uint32_t PageSizeFromComboSel(int sel, uint32_t cur)
{
    return ComboSelToValue(sel, kPgSizeOpts, 4, cur);
}
inline int PageSizeComboIndex(uint32_t v)
{
    return ComboValueToSel(v, kPgSizeOpts, 4);
}

} // namespace lingxi
