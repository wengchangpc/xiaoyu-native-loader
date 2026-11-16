// log.cpp
//
// 这里刻意**不使用 CRT 的文件流**，原因是踩过一次硬坑：
//
//   _wfopen(path, L"a, ccs=UTF-8") 打开的是"宽定向流"，
//   之后再对它调 fprintf / fwrite / fputc 写窄字符，属于未定义行为。
//   实测表现：文件被创建、3 字节 UTF-8 BOM 已经落盘，
//   然后第一行日志就把整个进程带走 —— 现场只剩 EF BB BF。
//
// 所以改成纯 Win32：每次 CreateFileW(FILE_APPEND_DATA) -> WriteFile -> CloseHandle。
// 无缓冲、无定向、不依赖 CRT 内部状态，DllMain 早期和 loader lock 下都能安全调用。
//
// 另外：每次写完立刻 CloseHandle，进程即使下一秒崩掉，最后一行也一定在磁盘上。
#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>   // wcscpy_s / wcscat_s / wcslen

namespace xiaoyu {
namespace {

// SRWLOCK 可以静态初始化，不需要任何 Init 调用。
// 比"懒初始化 CRITICAL_SECTION + 标志位"更省事，也不会有竞态。
SRWLOCK g_lock = SRWLOCK_INIT;

wchar_t g_path[MAX_PATH * 2] = {0};
bool g_ready = false;

// 从末尾往前找分隔符，逐级建目录（CreateDirectoryW 对已存在目录返回 FALSE，忽略即可）
void EnsureDir(const wchar_t* file_path) {
    wchar_t buf[MAX_PATH * 2];
    size_t n = wcslen(file_path);
    if (n >= MAX_PATH * 2) return;
    memcpy(buf, file_path, (n + 1) * sizeof(wchar_t));
    for (wchar_t* p = buf + n; p > buf; --p) {
        if (*p == L'\\' || *p == L'/') {
            *p = L'\0';
            if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) break;
            CreateDirectoryW(buf, nullptr);
        }
    }
}

// 打开（不存在则创建）日志文件。返回 INVALID_HANDLE_VALUE 表示不可用。
HANDLE OpenForAppend() {
    return CreateFileW(g_path, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// 底层写：整块 buffer 一次 WriteFile，不再拼时间戳前缀。
void RawWrite(const char* data, int len) {
    if (data == nullptr || len <= 0) return;

    // 先发 DebugView 通道：即使文件写不进去，用 DbgView / DebugView++ 也能看到。
    // 这里必须自己补 '\0'，OutputDebugStringA 要 null-terminated。
    {
        char dbg[2060];
        int n = len < (int)sizeof(dbg) - 3 ? len : (int)sizeof(dbg) - 3;
        memcpy(dbg, data, (size_t)n);
        dbg[n] = '\n';
        dbg[n + 1] = '\0';
        OutputDebugStringA(dbg);
    }

    if (!g_ready) return;

    AcquireSRWLockExclusive(&g_lock);

    HANDLE h = OpenForAppend();
    if (h == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    // 新文件开头补 BOM，Notepad / VS Code 才能正确识别成 UTF-8
    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
        DWORD w = 0;
        WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
    }

    DWORD written = 0;
    WriteFile(h, data, (DWORD)len, &written, nullptr);
    // 补换行：调用方只管内容，行尾统一由这里负责
    WriteFile(h, "\r\n", 2, &written, nullptr);

    CloseHandle(h);  // ← 立刻关，保证崩溃前的最后一行一定在盘上

    ReleaseSRWLockExclusive(&g_lock);
}

// 拼 "[HH:MM:SS.mmm] " 前缀
int MakeStamp(char* out, int cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return _snprintf_s(out, (size_t)cap, _TRUNCATE, "[%02d:%02d:%02d.%03d] ",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

}  // namespace

void LogInit() {
    if (g_ready) return;

    // 先试 C:\Xiaoyu\native.log
    wcscpy_s(g_path, L"C:\\Xiaoyu\\native.log");
    EnsureDir(g_path);

    HANDLE h = OpenForAppend();
    if (h == INVALID_HANDLE_VALUE) {
        // 退回 %TEMP%
        wchar_t tmp[MAX_PATH] = {0};
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0) {
            wcscpy_s(g_path, tmp);
            wcscat_s(g_path, L"xiaoyu_native.log");
            h = OpenForAppend();
        }
    }

    if (h == INVALID_HANDLE_VALUE) {
        // 两个位置都写不了：保持 g_ready = false，后续所有写操作全部 no-op
        return;
    }
    CloseHandle(h);

    g_ready = true;
    LogLine("================ xiaoyu native attach ================");
}

void LogClose() {
    // 没有长期持有句柄，这里只需要关闭开关；
    // 不把 g_path 清掉，方便崩溃后外部脚本读 LogPath() 定位文件。
    g_ready = false;
}

void LogLine(const char* fmt, ...) {
    if (!g_ready || fmt == nullptr) return;

    char body[1900];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;  // 被截断或格式化失败

    // 时间戳 + 正文拼成一整块再一次写出，避免两段之间被别的线程插进来
    char line[2048];
    int off = MakeStamp(line, (int)sizeof(line));
    if (off < 0) off = 0;
    int room = (int)sizeof(line) - off;
    if (n > room) n = room;
    memcpy(line + off, body, (size_t)n);

    RawWrite(line, off + n);
}

void LogLineW(const wchar_t* text) {
    if (!g_ready || text == nullptr) return;

    char body[1900];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, body, (int)sizeof(body) - 1,
                                nullptr, nullptr);
    if (n <= 1) return;  // 0/1 表示失败或空串

    char line[2048];
    int off = MakeStamp(line, (int)sizeof(line));
    if (off < 0) off = 0;
    int len = n - 1;  // 去掉结尾的 '\0'
    int room = (int)sizeof(line) - off;
    if (len > room) len = room;
    memcpy(line + off, body, (size_t)len);

    RawWrite(line, off + len);
}

const wchar_t* LogPath() { return g_path; }

}  // namespace xiaoyu
