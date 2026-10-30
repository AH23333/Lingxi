#pragma once
//
// 候选窗拆分（P2 重构，2026-10-31）后的文件间内部共享面：DPI 缩放与
// 工具条几何（绘制 candwnd_render.cpp 与命中 candwnd_interact.cpp 共用
// 单一定义）。仅 candwnd_core/render/interact.cpp 包含，不得外扩。
//
#include <windows.h>

namespace lingxi {

// ---- 底部工具条几何（绘制与命中共用单一定义；单位 96dpi，内部 DPI 缩放）----
// 左组（多页时）：‹(20) 页码(46) ›(20)，起于 x=6；
// 右组：固(28) 符(28) 设(28)，止于 x=右-6；中段空白=拖拽手柄。
inline UINT Dpi()
{
    UINT dpi = 96;
    if (auto p = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                 "GetDpiForSystem"))
        dpi = p();
    return dpi;
}

inline int Scale96(int v) { return MulDiv(v, (int)Dpi(), 96); }

inline void CalcToolbarRects(int cw, int ch, int tbH, bool paged,
                             RECT* rPrev, RECT* rPg, RECT* rNext,
                             RECT* rPin, RECT* rSym, RECT* rSet)
{
    int top = ch - tbH + 2;
    int bot = ch - 2;
    auto mk = [top, bot](int x96, int w96) -> RECT
    {
        int x = Scale96(x96), w = Scale96(w96);
        return RECT{ x, top, x + w, bot };
    };
    RECT empty{ 0, 0, 0, 0 };
    if (rPrev) *rPrev = paged ? mk(6, 20) : empty;
    if (rPg)   *rPg   = paged ? mk(28, 46) : empty;
    if (rNext) *rNext = paged ? mk(76, 20) : empty;
    // 右组从右往左：设(0) 符(1) 固(2)，像素直接换算（cw 已是缩放后像素）
    int m = Scale96(6), bw = Scale96(28), g = Scale96(2);
    auto rgt = [&](int fromRight) -> RECT
    {
        int x = cw - m - bw - fromRight * (bw + g);
        return RECT{ x, top, x + bw, bot };
    };
    if (rPin)  *rPin  = rgt(2);
    if (rSym)  *rSym  = rgt(1);
    if (rSet)  *rSet  = rgt(0);
}

} // namespace lingxi
