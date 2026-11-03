#pragma once
//
// ime/keygate.h — 数字键选词认领闸（审查缺陷②：Test/Down 页宽不对称）。
// 统一口径：页外数字一律放行给宿主——Test 侧三处认领（组合态
// composition.cpp PinyinKeyEaten、直通道 IsSelectionKey、联想撤销门
// keepAssoc）与 Down 侧选词闸（(wParam-'1') < candPageSize）共用本谓词。
// 修复前页宽 3 按 5：键被吃掉、不选词、还把拼音串 PinyinCommitRaw 上屏。
// engine_test DG 块直链钉真值表（页宽 3/1/9）；真机按键手感 blocked。
//
#include <cstdint>

namespace lingxi {

// wParam 为 1-9 主行数字且 off 落在页内（off = wParam - '1' < pageSize）
// 才认领选词。页宽 9 时 off 恒 <9——与历史行为零差异（批3 基线）。
inline bool DigitSelectClaim(uint32_t wParam, uint32_t pageSize)
{
    if (wParam < L'1' || wParam > L'9') return false;
    return (wParam - L'1') < pageSize;
}

} // namespace lingxi
