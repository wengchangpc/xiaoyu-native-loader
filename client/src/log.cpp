// log.cpp
#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>

namespace xiaoyu {
namespace {

FILE* g_file = nullptr;
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;
wchar_t g_path[MAX_PATH * 2] = {0};

void EnsureDir(const wchar_t* path) {
    // 从末尾往前找分隔符，逐级建目录
    wchar_t buf[MAX_PATH * 2];
    wcscpy_s(buf, path);
    for (wchar_t* p = buf + wcslen(buf); p > buf; --p) {
        if (*p == L'\\') {
            *p = L'\0';
            if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) break;
            CreateDirectoryW(buf, nullptr);
        }
    }
}

void WriteRaw(const char* data, int len) {
    if (g_file == nullptr || len <= 0) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    fwrite(data, 1, (size_t)len, g_file);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_cs);
}

}  // namespace

void LogInit() {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
    if (g_file != nullptr) return;

    wcscpy_s(g_path, L"C:\\Xiaoyu\\native.log");
    EnsureDir(g_path);
    g_file = _wfopen(g_path, L"a, ccs=UTF-8");
    if (g_file == nullptr) {
        wchar_t tmp[MAX_PATH * 2];
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0) {
            wcscat_s(tmp, L"xiaoyu_native.log");
            wcscpy_s(g_path, tmp);
            g_file = _wfopen(g_path, L"a, ccs=UTF-8");
        }
    }
    if (g_file != nullptr) {
        LogLine("================ xiaoyu native attach ================");
    }
}

void LogClose() {
    if (g_file != nullptr) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void LogLine(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    WriteRaw(buf, n);
}

void LogLineW(const wchar_t* text) {
    if (text == nullptr) return;
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf) - 1,
                                nullptr, nullptr);
    if (n <= 0) return;
    WriteRaw(buf, n - 1);
}

const wchar_t* LogPath() { return g_path; }

}  // namespace xiaoyu
