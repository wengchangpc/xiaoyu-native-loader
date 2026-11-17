// bridge.cpp - 见 bridge.h
#include "bridge.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "log.h"
#include "memory.h"

namespace xiaoyu {
namespace bridge {
namespace {

const wchar_t* kDir = L"C:\\Xiaoyu";
const wchar_t* kCmdPath = L"C:\\Xiaoyu\\run.bat";
const wchar_t* kDonePath = L"C:\\Xiaoyu\\run.done";
const wchar_t* kOutPath = L"C:\\Xiaoyu\\bridge.log";

FILE* g_pf = nullptr;
volatile LONG g_started = 0;
volatile LONG g_exec_count = 0;

void POpen() {
    if (g_pf != nullptr) return;
    CreateDirectoryW(kDir, nullptr);
    g_pf = fopen("C:\\Xiaoyu\\bridge.log", "wb");
}

void P(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_pf != nullptr) {
        fputs(buf, g_pf);
        fputc('\n', g_pf);
        fflush(g_pf);
    }
    LogLine("[bridge] %s", buf);
}

// 执行 run.bat。返回 true 表示确实跑了一次。
bool RunOnce() {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    // 不显示控制台窗口 —— 免得游戏前台莫名闪一个黑框
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    wchar_t cmdline[MAX_PATH * 2];
    swprintf_s(cmdline, L"cmd.exe /c \"%s\"", kCmdPath);

    // CREATE_NO_WINDOW：子进程不分配控制台。
    // 注意这里**不加** CREATE_BREAKAWAY_FROM_JOB：游戏进程不一定在 job 里，
    // 加了反而可能因为 ERROR_ACCESS_DENIED 直接失败。
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, kDir, &si, &pi)) {
        const DWORD err = GetLastError();
        P("CreateProcess 失败 err=%lu", (unsigned long)err);
        // 失败了也要把文件挪走，否则会每 500ms 重试一次，刷爆日志
        MoveFileExW(kCmdPath, kDonePath, MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    P("run.bat 已启动 pid=%lu，等待结束...", (unsigned long)pi.dwProcessId);

    // 最多等 10 分钟。侦察脚本通常几秒，留足余量但不至于永久卡住。
    const DWORD wait = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (wait == WAIT_TIMEOUT) {
        P("run.bat 超时（10 分钟），放弃等待");
    } else {
        P("run.bat 结束 exit=%lu", (unsigned long)code);
    }

    MoveFileExW(kCmdPath, kDonePath, MOVEFILE_REPLACE_EXISTING);
    InterlockedIncrement(&g_exec_count);
    return true;
}

DWORD WINAPI ThreadProc(LPVOID param) {
    (void)param;
    POpen();
    CreateDirectoryW(kDir, nullptr);
    P("命令通道就绪，监听 %ls", kCmdPath);
    P("用法：往 C:\\Xiaoyu\\run.bat 写一个 bat，本线程会以游戏进程的权限执行它");

    for (;;) {
        const DWORD attr = GetFileAttributesW(kCmdPath);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            RunOnce();
        }
        Sleep(500);
    }
    return 0;
}

}  // namespace

void Start() {
    if (InterlockedExchange(&g_started, 1) != 0) return;
    HANDLE th = CreateThread(nullptr, 0, &ThreadProc, nullptr, 0, nullptr);
    if (th != nullptr) CloseHandle(th);
}

int ExecCount() { return (int)g_exec_count; }

}  // namespace bridge
}  // namespace xiaoyu
