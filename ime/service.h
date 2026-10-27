#pragma once
//
// CTextService —— 灵犀输入法的 TSF 文本服务本体。
//
// 线程模型：实例创建与所有 TSF 调用都在宿主应用的 UI 线程（TSF 保证）；
// 管道读线程只允许：更新候选缓存（临界区内）、向封送窗口 PostMessage。
// 任何情况下不阻塞宿主线程超过 80ms（管道写入超时上限）。
//
#include <windows.h>
#include <msctf.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "pipeclient.h"

extern HINSTANCE g_hInst;

namespace lingxi {

constexpr LONG kCtxMaxChars = 300;          // 上下文窗口（光标前字符数）
// WM_APP_LX_PICK 定义于 common/proto.h
constexpr UINT WM_APP_LX_MODE = WM_APP + 2; // 封送窗口：消息泵空闲点安全检测拼音模式

class CTextService :
    public ITfTextInputProcessorEx,
    public ITfThreadMgrEventSink,
    public ITfTextEditSink,
    public ITfKeyEventSink,
    public ITfThreadFocusSink,
    public ITfCompositionSink,
    public IPipeClientCallback
{
public:
    CTextService();
    ~CTextService();

    // ---- IUnknown ----
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // ---- ITfTextInputProcessor / Ex ----
    STDMETHOD(Activate)(ITfThreadMgr* pThreadMgr, TfClientId tid) override;
    STDMETHOD(Deactivate)() override;
    STDMETHOD(ActivateEx)(ITfThreadMgr* pThreadMgr, TfClientId tid, DWORD dwFlags) override;

    // ---- ITfThreadMgrEventSink ----
    STDMETHOD(OnInitDocumentMgr)(ITfDocumentMgr* pDocMgr) override;
    STDMETHOD(OnUninitDocumentMgr)(ITfDocumentMgr* pDocMgr) override;
    // SDK 语义：第一参数 = 获得焦点的文档，第二参数 = 失去焦点的文档
    STDMETHOD(OnSetFocus)(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
    STDMETHOD(OnPushContext)(ITfContext* pContext) override;
    STDMETHOD(OnPopContext)(ITfContext* pContext) override;

    // ---- ITfTextEditSink ----
    STDMETHOD(OnEndEdit)(ITfContext* pContext, TfEditCookie ecReadOnly,
                         ITfEditRecord* pEditRecord) override;

    // ---- ITfKeyEventSink ----
    STDMETHOD(OnSetFocus)(BOOL fForeground) override;
    STDMETHOD(OnTestKeyDown)(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHOD(OnKeyDown)(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHOD(OnTestKeyUp)(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHOD(OnKeyUp)(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHOD(OnPreservedKey)(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) override;

    // ---- ITfThreadFocusSink ----
    STDMETHOD(OnSetThreadFocus)() override;
    STDMETHOD(OnKillThreadFocus)() override;

    // ---- ITfCompositionSink（拼音组合被宿主强制终止时的清理）----
    STDMETHOD(OnCompositionTerminated)(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ---- IPipeClientCallback（管道读线程调用，禁止触碰 TSF）----
    void OnCandidates(uint32_t ver, uint32_t replaceLen,
                      std::vector<std::wstring>&& cands) override;
    void OnPickRequested(uint32_t ver, uint32_t index) override; // 仅 PostMessage

    // ---- 宿主 UI 线程入口 ----
    void HandlePickFromUI(uint32_t ver, uint32_t index); // 封送窗口消息
    void OnContextRead(const std::wstring& text, const RECT& rc); // 编辑会话回调

    // ---- 键盘选词 / 状态 ----
    bool CandidatesVisible();
    void InsertCandidate(uint32_t index);
    void ClearCandidates();
    void SendHide();
    void AbortCompositionState(); // 组合操作异常后安全复位，并在宿主结束悬挂组合
    void AbortHostCompositionInner(ITfRange* pRange, ITfComposition* pComp); // SEH 内层 C++ 实现

    // ---- M5 拼音模式（zh-CN profile 激活时启用）----
    void DetectPinyinMode();
    bool IsPinyinMode() const { return _pinyinMode; }
    // 是否正在拼音输入（有宿主组合，或无组合模式下内存拼音串非空）。
    bool PinyinActive() const { return _pComposition || (!_inlineComp && !_pinyin.empty()); }
    void HandlePinyinKey(ITfContext* pContext, WPARAM wParam, BOOL* pfEaten); // OnKeyDown 分发
    bool PinyinKeyEaten(WPARAM wParam);                                       // OnTestKeyDown 判定
    bool GetCandidate(uint32_t index, std::wstring& out);                     // 候选缓存读取
    void SendPinyinCtx();                                                     // 组合串+光标矩形→UI

private:
    friend class CReadSession;
    friend class CInsertSession;
    friend class CReplaceSession;
    friend class CPinyinEditSession; // 组合窗口编辑会话（composition.cpp）

    // 编辑动作（内部经 CPinyinEditSession 在宿主线程执行）
    void PinyinStart(wchar_t ch);
    void PinyinAppend(wchar_t ch);
    void PinyinShrink();
    void PinyinCancel();
    void PinyinCommitHanzi(const std::wstring& hanzi);
    void PinyinCommitRaw();
    void PinyinResetNoInline(); // 无组合模式收尾：清内存状态并隐藏候选窗
    void PinyinEndComposition(ITfContext* pCtx, const std::wstring* replaceWith); // 共用收尾

    bool InitSinks();
    void UninitSinks();
    bool InitTextEditSink(ITfDocumentMgr* pDocMgr); // TRUE=继续
    void RequestContextRead(ITfContext* pContext);
    bool InsertTextAtSelection(const std::wstring& text);
    bool ReplaceTextBeforeCaret(const std::wstring& text, uint32_t deleteLen);
    void SendCtx(const std::wstring& text, const RECT& rc, uint8_t flags = 0);

    LONG _cRef = 1;
    ITfThreadMgr* _pThreadMgr = nullptr;
    TfClientId _tfClientId = 0;
    BOOL _fForeground = FALSE;

    DWORD _tmSinkCookie = TF_INVALID_COOKIE;
    DWORD _tfSinkCookie = TF_INVALID_COOKIE;
    DWORD _editSinkCookie = TF_INVALID_COOKIE;
    LANGID _lastLangid = 0; // 按键路径语言变化检测
    bool _cachedPinyinEnabled = true; // 按键路径设置开关变化检测（托盘/设置页改动即时生效）
    ITfContext* _pEditSinkCtx = nullptr; // 持有引用
    BOOL _fSelfInsert = FALSE;           // 自己上屏引发的编辑，忽略其 OnEndEdit
    // 自插入编辑会话在 Chromium 上常异步执行，其 OnEndEdit 晚于 _fSelfInsert 复位，
    // 用时间戳兜底抑制这一滞后回调，避免上屏后又误弹上下文预测。
    ULONGLONG _lastSelfInsertTick = 0;

    // 候选缓存（_cs 保护；读线程写、UI 线程读写）
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    std::vector<std::wstring> _cands;
    uint32_t _verSent = 0;     // 最近发出的上下文版本
    uint32_t _verCands = 0;    // 候选对应的版本
    uint32_t _replaceLen = 0;  // 上屏前需删除的光标前字符数
    bool _fActivated = false;

    HWND _hwndMarshal = nullptr; // 隐藏封送窗口（宿主 UI 线程）
    static LRESULT CALLBACK MarshalWndProc(HWND, UINT, WPARAM, LPARAM);
    void RequestModeDetect();       // 置脏 + 投递封送消息（消息泵空闲点检测）
    void MaybeRedetectPinyinMode(); // 按键路径：语言变化 → RequestModeDetect

    // 拼音模式：激活/焦点/TIP 前台/语言变化只投递封送消息，实际检测在消息泵
    // 空闲点执行（远离 CTF 激活回调与按键分发——见 PITFALLS #17/#18）
    bool _pinyinMode = false;
    std::atomic<bool> _modeDirty{ false }; // 语言/前台/焦点变化置位，按键路径执行检测
    bool _fInPinyinEdit = false;  // 组合编辑会话执行中（抑制 OnEndEdit 自激发）
    std::wstring _pinyin;         // 当前拼音字母串
    ITfComposition* _pComposition = nullptr;
    ITfRange* _compRange = nullptr;
    // 内联组合（把拼音字母写入宿主 composition）。Chromium 对组合的写操作/销毁
    // 不稳定，一旦异常残留悬挂组合会污染整个 TSF 上下文（连带微软拼音）。
    // 默认 false=无组合模式：拼音只在候选窗显示、不写入宿主，宿主从不产生组合，
    // 从根本上杜绝悬挂组合污染。
    bool _inlineComp = false;

    CPipeClient _pipe;
};

// composition.cpp 提供（SEH 包裹的模式检测，供激活回调安全调用）
BOOL DetectPinyinModeSEH(CTextService* svc);
// composition.cpp 提供：拼音组合键处理全程 SEH 包裹——组合编辑会话在 CTF/TSF
// 框架内部执行，宿主状态突变（语言切换、文档拆除）可能触发框架内部 AV，
// TIP 必须遏制而不能带崩宿主进程
void HandlePinyinKeySEH(CTextService* svc, ITfContext* pContext, WPARAM wParam, BOOL* pfEaten);

} // namespace lingxi
