#pragma once
//
// IPC 协议（ImeCore.dll <-> LingxiUI.exe，命名管道）。
//
// 帧 = [u32 bodyLen][u8 type][body]，bodyLen 不含长度字段本身。
// 字符串编码 = [u16 charCount][wchar_t x charCount]（原生 UTF-16）。
// 所有消息带上下文版本号 ver：DLL 每次发出新上下文递增；
// 过期的候选/上屏请求一律丢弃，防止跨应用串台。
//
// 说明：实现采用了定长二进制帧而非规划稿中的 JSON 行——消息结构固定且
// 只有我们两个二进制使用，二进制省去转义与解析边界问题，代码量减半。
// 协议语义（版本化消息、命名管道）与规划一致。
//
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace lingxi {

constexpr uint32_t kProtoVer = 1;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\lingxi-ime-ipc";

// DLL 内隐藏封送窗口消息：wParam=index, lParam=ver
constexpr UINT WM_APP_LX_PICK = WM_APP + 1;
// 封送窗口：插入任意文本（符号面板）。lParam=堆分配的 std::wstring*，UI 线程取出后 delete
constexpr UINT WM_APP_LX_INSERT = WM_APP + 3;

// 候选分页：数字键 1-9 对应一页内的 9 个候选，翻页键切换页。
constexpr uint32_t kPageSize = 9;
// 引擎一次返回、经 IPC 传递的最大候选数（足够翻 6 页以上）。
constexpr uint32_t kMaxCandidates = 60;

// 纯函数：由候选总数、当前页起始、翻页方向(delta=±1) 求新页起始，已夹到
// 合法范围（首页 0；末页起点为 total 内最后一个整页起点）。total=0 → 0。
inline uint32_t NextPageStart(size_t total, uint32_t cur, int delta)
{
    if (total == 0) return 0;
    size_t last = (total - 1) / kPageSize * kPageSize;
    long n = (long)cur + (long)kPageSize * delta;
    if (n < 0) n = 0;
    if ((size_t)n > last) n = (long)last;
    return (uint32_t)n;
}

enum MsgType : uint8_t
{
    MSG_HELLO = 1,      // DLL→UI  连接建立
    MSG_ACTIVATE = 2,   // DLL→UI  TIP 激活
    MSG_DEACTIVATE = 3, // DLL→UI  TIP 停用
    MSG_HIDE = 4,       // DLL→UI  隐藏候选窗（失焦等）
    MSG_CTX = 5,        // DLL→UI  上下文更新，触发预测
    MSG_CANDS = 6,      // UI→DLL  全量候选列表（UI 窗口据此分页显示）
    MSG_PICK = 7,       // UI→DLL  鼠标点击候选，请求上屏（index 为全量全局索引）
    MSG_PAGE = 8,       // DLL→UI  翻页：请求显示 pageStart 起始的那一页
    MSG_PICKED = 9,     // DLL→UI  键盘选字反馈（空格/数字），用于自学习（ver+index）
    MSG_MODE_TOAST = 10,// DLL→UI  中/英文模式切换的短暂提示（body=u8 english）
    MSG_INSERT_TEXT = 11,// UI→DLL 插入任意文本（符号面板点击）；body=字符串
};

#pragma pack(push, 1)
struct MsgHello
{
    uint32_t protoVer;
    uint32_t pid;          // 宿主进程 PID
    uint8_t darkTheme;     // 系统深色模式
};

struct MsgCtx
{
    uint32_t ver;
    int32_t caretX, caretY, caretW, caretH; // 屏幕坐标，取不到时为 0
    uint8_t flags;                          // bit0: 拼音组合模式（text 为拼音字母串）
    uint8_t reserved[3];
    uint32_t ctxChars;                      // bit1 置位时存在：跟随 ctxChars 个 wchar_t（已上屏上文，LLM 重排用）
    uint32_t textChars;                     // 跟随 textChars 个 wchar_t
};

// MsgCtx.flags 位定义
constexpr uint8_t CTX_F_PINYIN = 0x01;
constexpr uint8_t CTX_F_CTX_PREFIX = 0x02;
// CTX_F_ASSOC（GAP §11 选词联想）：上屏后 DLL 主动发起的联想帧——text=光标前文
// （非空）、不带 PINYIN 位。旧 UI 把它当普通续写帧照常链预测（天然向后兼容）；
// 新 UI 侧行为同续写，标志只在 DLL 侧用于联想态记账（撤销/防双发/连发）。
constexpr uint8_t CTX_F_ASSOC = 0x04;

// 旧版 MsgCtx（无 ctxChars）：宿主进程里驻留的上一版 DLL 仍按此格式发帧
// （能改名不能覆盖，重建后老 DLL 继续跑到进程退出），接收端必须兼容。
struct MsgCtxV1
{
    uint32_t ver;
    int32_t caretX, caretY, caretW, caretH;
    uint8_t flags;
    uint8_t reserved[3];
    uint32_t textChars;
};

struct MsgCands
{
    uint32_t ver;
    uint32_t count;         // 跟随 count 组 [u16 charLen][wchar_t...]
    uint32_t replaceLen;    // 上屏前需删除光标前的字符数（英文前缀补全）
};
// B1 部分提交：MsgCands 的 count 组字符串之后可选择性跟随 count 个 u16 cover
// （各候选消耗的原始拼音前缀字符数）。写入端仅在 cover 与候选平行且有效时追加；
// 读取端顺序读完字符串后：能读到 count 个 u16 即采用，读不到则全 0（=旧行为，
// 提交后全清）。新旧两侧双向兼容，kProtoVer 不动。

struct MsgPick
{
    uint32_t ver;
    uint32_t index;
};

struct MsgPage
{
    uint32_t ver;
    uint32_t pageStart; // 本页首个候选在全量列表中的全局索引（0/9/18...）
};
#pragma pack(pop)

inline void AppendStr(std::vector<uint8_t>& v, const std::wstring& s)
{
    uint16_t n = (uint16_t)min(s.size(), 0xFFFFu);
    const uint8_t* p = (const uint8_t*)&n;
    v.insert(v.end(), p, p + sizeof(n));
    p = (const uint8_t*)s.data();
    v.insert(v.end(), p, p + (size_t)n * sizeof(wchar_t));
}

inline bool ReadStr(const uint8_t*& p, const uint8_t* end, std::wstring& s)
{
    if (p + sizeof(uint16_t) > end) return false;
    uint16_t n;
    memcpy(&n, p, sizeof(n));
    p += sizeof(n);
    if (p + (size_t)n * sizeof(wchar_t) > end) return false;
    s.assign((const wchar_t*)p, n);
    p += (size_t)n * sizeof(wchar_t);
    return true;
}

template <typename T>
inline void AppendPod(std::vector<uint8_t>& v, const T& pod)
{
    const uint8_t* p = (const uint8_t*)&pod;
    v.insert(v.end(), p, p + sizeof(pod));
}

template <typename T>
inline bool ReadPod(const uint8_t*& p, const uint8_t* end, T& pod)
{
    if (p + sizeof(T) > end) return false;
    memcpy(&pod, p, sizeof(T));
    p += sizeof(T);
    return true;
}

} // namespace lingxi
