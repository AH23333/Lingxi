#pragma once
//
// 命名管道通信辅助：服务端实例创建、客户端连接、帧读写。
// 全部使用 overlapped IO + 超时，保证调用方（尤其是宿主 UI 线程）不被阻塞。
//
#include <windows.h>
#include <cstdint>
#include <vector>

#include "proto.h"

namespace lingxi {

// 服务端：创建一个管道实例（第一个实例加 FILE_FLAG_FIRST_PIPE_INSTANCE 防多开）。
HANDLE CreateLingxiPipeServerInstance(bool firstInstance);

// 客户端：单次连接尝试（不循环；重试逻辑由调用方线程负责）。
HANDLE OpenLingxiPipeClient();

// 发送一帧。超时或失败返回 false（调用方决定丢弃还是断开）。
bool WriteFrame(HANDLE hPipe, uint8_t type, const void* body, uint32_t bodyLen, uint32_t timeoutMs);

// 读取一帧。阻塞直到完整帧到达、hStop 被触发或管道出错。
// 返回 false 表示应结束读取循环（出错/停止）。
bool ReadFrame(HANDLE hPipe, HANDLE hStop, uint8_t& type, std::vector<uint8_t>& body);

// 便捷封装：帧体 = body 后跟一个字符串。
bool WriteFrameStr(HANDLE hPipe, uint8_t type, const void* prefixBody, uint32_t prefixLen,
                   const std::wstring& str, uint32_t timeoutMs);

} // namespace lingxi
