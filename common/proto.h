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

enum MsgType : uint8_t
{
    MSG_HELLO = 1,      // DLL→UI  连接建立
    MSG_ACTIVATE = 2,   // DLL→UI  TIP 激活
    MSG_DEACTIVATE = 3, // DLL→UI  TIP 停用
    MSG_HIDE = 4,       // DLL→UI  隐藏候选窗（失焦等）
    MSG_CTX = 5,        // DLL→UI  上下文更新，触发预测
    MSG_CANDS = 6,      // UI→DLL  候选列表（UI 窗口同时据此显示）
    MSG_PICK = 7,       // UI→DLL  鼠标点击候选，请求上屏
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
    uint32_t textChars;                     // 跟随 textChars 个 wchar_t
};

// MsgCtx.flags 位定义
constexpr uint8_t CTX_F_PINYIN = 0x01;

struct MsgCands
{
    uint32_t ver;
    uint32_t count;         // 跟随 count 组 [u16 charLen][wchar_t...]
    uint32_t replaceLen;    // 上屏前需删除光标前的字符数（英文前缀补全）
};

struct MsgPick
{
    uint32_t ver;
    uint32_t index;
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
