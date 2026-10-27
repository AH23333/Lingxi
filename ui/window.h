#pragma once
//
// 候选窗：置顶、无边框、WS_EX_NOACTIVATE（点击不抢宿主焦点）。
// 数据由管道客户端线程写入（临界区保护），绘制在 UI 主线程。
//
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace lingxi {

class CandWindow
{
public:
    bool Init(HINSTANCE hInst);
    void Shutdown();

    // caretScreen：光标屏幕矩形（全 0 表示未知，沿用上次位置）
    // cands 为全量候选（窗口内部分页，每页 kPageSize 个），ver 变化时回到第一页。
    void Show(const RECT& caretScreen, const std::vector<std::wstring>& cands,
              bool dark, uint32_t ver);
    void Hide();
    // 翻页：显示全量列表中 pageStart 起始的那一页（ver 不匹配则忽略）。
    void SetPage(uint32_t ver, uint32_t pageStart);
    // 中/英文模式切换的短暂提示：在光标附近显示“中/英”，约 1 秒后自动消失。
    void ShowModeToast(bool english);

    void SetPickHandler(std::function<void(uint32_t ver, uint32_t index)> handler)
    {
        _pick = std::move(handler);
    }

private:
    static LRESULT CALLBACK s_WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    void LayoutAndPosition();
    void LayoutMetrics(int* itemH, int* pad);
    // 按字号（DPI 缩放）重建三支字体；先删除旧字体，避免泄漏
    void RebuildFonts(uint32_t fontSize);
    // 应用皮肤（强调色 + 浅色 hover 底色）；越界回退默认蓝
    void ApplySkin(uint32_t idx);
    // 调用方持锁：返回当前页在全量列表中的起止 [begin,end)
    void CurrentPageRangeLocked(size_t* begin, size_t* end) const;

    HWND _hwnd = nullptr;
    HFONT _font = nullptr;
    HFONT _toastFont = nullptr; // toast 大字（进程内复用）
    HFONT _smallFont = nullptr; // 底部页码/标记小字
    uint32_t _fontSize = 16;    // 当前字号（变化时重建字体）
    uint32_t _skinIndex = 0;    // 当前皮肤（变化时换强调色）
    COLORREF _accent = RGB(0, 120, 215);   // 首行/toast 底色（皮肤）
    COLORREF _hoverLight = RGB(224, 238, 252); // 浅色 hover 底色（皮肤）
    bool _dark = true;
    RECT _caret{};          // 屏幕坐标
    uint32_t _ver = 0;
    std::vector<std::wstring> _cands; // 全量候选
    uint32_t _pageStart = 0;          // 当前页首个候选的全局索引
    int _width = 0, _height = 0;
    int _lastX = 120, _lastY = 120;
    bool _visible = false;
    bool _toastMode = false;   // 正在显示中/英文模式提示
    bool _toastEnglish = false;
    int _hoverRow = -1;        // 鼠标悬停的页内行号（-1 无），用于高亮
    bool _trackingMouse = false;
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    std::function<void(uint32_t, uint32_t)> _pick;
};

} // namespace lingxi
