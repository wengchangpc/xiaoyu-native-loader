// log.cpp
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

static char g_log_path[MAX_PATH] = {0};
static CRITICAL_SECTION g_log_cs;
static bool g_log_cs_ready = false;

static void EnsureDir(const char* dir) {
    DWORD attr = GetFileAttributesA(dir);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA(dir, nullptr);
    }
}

static void InitPath() {
    if (g_log_path[0]) return;
    // 优先 C:\Xiaoyu\loader.log（和客户端其它产物放一起）
    EnsureDir("C:\\Xiaoyu");
    const char* preferred = "C:\\Xiaoyu\\loader.log";
    HANDLE h = CreateFileA(preferred, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        strncpy(g_log_path, preferred, MAX_PATH - 1);
        return;
    }
    // 退回到 %TEMP%
    char tmp[MAX_PATH] = {0};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) {
        _snprintf(g_log_path, MAX_PATH - 1, "%sxiaoyu_loader.log", tmp);
        g_log_path[MAX_PATH - 1] = 0;
    } else {
        strncpy(g_log_path, "C:\\xiaoyu_loader.log", MAX_PATH - 1);
    }
}

const char* LogPath() {
    InitPath();
    return g_log_path;
}

void LogLine(const char* fmt, ...) {
    InitPath();
    if (!g_log_cs_ready) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = true;
    }

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2304];
    _snprintf(line, sizeof(line) - 1, "[%02d:%02d:%02d.%03d] %s\r\n",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    line[sizeof(line) - 1] = 0;

    EnterCriticalSection(&g_log_cs);
    HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)strlen(line), &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_cs);

    OutputDebugStringA(line);
}
