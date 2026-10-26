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
constexpr UINT WM_APP_LX_MODE = WM_APP + 2; // 封送窗口：延迟执行拼音模式检测

class CTextService :
    public ITfTextInputProcessorEx,
    public ITfThreadMgrEventSink,
    public ITfTextEditSink,
    public ITfKeyEventSink,
    public ITfThreadFocusSink,
    public ITfCompositionSink,
    public ITfInputProcessorProfileActivationSink,
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

    // ---- ITfInputProcessorProfileActivationSink（profile 切换 → 重判拼音/直通模式）----
    STDMETHOD(OnActivated)(DWORD dwProfileType, LANGID langid, REFCLSID rclsid,
                           REFGUID catid, REFGUID guidProfile, HKL hkl, DWORD dwFlags) override;

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

    // ---- M5 拼音模式（zh-CN profile 激活时启用）----
    void DetectPinyinMode();
    bool IsPinyinMode() const { return _pinyinMode; }
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
    DWORD _asSinkCookie = TF_INVALID_COOKIE; // profile 激活通知
    DWORD _editSinkCookie = TF_INVALID_COOKIE;
    ITfContext* _pEditSinkCtx = nullptr; // 持有引用
    BOOL _fSelfInsert = FALSE;           // 自己上屏引发的编辑，忽略其 OnEndEdit

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
    static DWORD WINAPI s_ModeThreadProc(LPVOID);
    void ModeThreadProc();

    // 拼音组合状态
    bool _pinyinMode = false;
    std::atomic<bool> _modeDirty{ false }; // OnActivated 置位，模式轮询线程执行检测
    bool _fInPinyinEdit = false;  // 组合编辑会话执行中（抑制 OnEndEdit 自激发）
    std::wstring _pinyin;         // 当前拼音字母串
    ITfComposition* _pComposition = nullptr;
    ITfRange* _compRange = nullptr;
    HANDLE _modeThread = NULL;    // 模式轮询线程（MTA，脱离消息泵）
    HANDLE _modeStopEv = NULL;

    CPipeClient _pipe;
};

// composition.cpp 提供（SEH 包裹的模式检测，供激活回调安全调用）
BOOL DetectPinyinModeSEH(CTextService* svc);

} // namespace lingxi
