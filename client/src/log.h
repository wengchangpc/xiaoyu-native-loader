// log.h - 极简文件日志
#pragma once

namespace xiaoyu {

// 初始化日志：写 C:\Xiaoyu\native.log，失败退回 %TEMP%
void LogInit();
void LogClose();

// 纯 ASCII 格式化（内部会转 UTF-8 写入）
void LogLine(const char* fmt, ...);

// 需要打印中文时用这个
void LogLineW(const wchar_t* text);

const wchar_t* LogPath();

}  // namespace xiaoyu
