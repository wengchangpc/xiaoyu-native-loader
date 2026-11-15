// log.h - 极简日志：文件 + OutputDebugString
#pragma once
#include <windows.h>

// 追加一行到日志文件（UTF-8），同时输出到调试器
void LogLine(const char* fmt, ...);

// 首次调用时决定日志路径，返回该路径（失败返回空串）
const char* LogPath();
