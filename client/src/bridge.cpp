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
volatile LONG g_stop = 0;
// 正在跑子进程。硬卸载前必须确认它是 0：否则子进程结束时会把控制权
// 交回一段已经被 FreeLibrary 释放掉的代码，直接崩进程。
volatile LONG g_busy = 0;
// 轮询线程的句柄。留着是为了 Stop() 能 join —— 线程还在 Sleep 里就把
// DLL 卸了，它醒过来会跳到已释放的代码段。
HANDLE g_thread = nullptr;

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
    InterlockedExchange(&g_busy, 1);

    // ★ 抢锁必须在**执行之前**：把 run.bat 原子改名成一个本进程专属的
    //   临时文件，改名成功才算抢到，然后执行那个临时文件。
    //
    //   以前是先 CreateProcess 再改名。问题是 cmd.exe 执行期间一直占着
    //   run.bat（它要边读边解析），MoveFileEx 直接失败，文件留在原地，
    //   下一轮轮询又把它执行一遍。热替换场景下这是致命的：新 DLL 的
    //   bridge 线程会看到旧 DLL 留下的 run.bat，再跑一次里面的
    //   "unload + inject"，把刚注入的新 DLL 又卸掉，最后什么都不剩。
    wchar_t tmp[MAX_PATH];
    swprintf_s(tmp, L"%s\\run_%lu.bat", kDir, (unsigned long)GetCurrentProcessId());
    if (!MoveFileExW(kCmdPath, tmp, MOVEFILE_REPLACE_EXISTING)) {
        // 抢不到说明另一个实例刚好拿走了，本轮直接让出
        InterlockedExchange(&g_busy, 0);
        return false;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    // 不显示控制台窗口 —— 免得游戏前台莫名闪一个黑框
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    wchar_t cmdline[MAX_PATH * 2];
    swprintf_s(cmdline, L"cmd.exe /c \"%s\"", tmp);

    // CREATE_NO_WINDOW：子进程不分配控制台。
    // 注意这里**不加** CREATE_BREAKAWAY_FROM_JOB：游戏进程不一定在 job 里，
    // 加了反而可能因为 ERROR_ACCESS_DENIED 直接失败。
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, kDir, &si, &pi)) {
        const DWORD err = GetLastError();
        P("CreateProcess 失败 err=%lu", (unsigned long)err);
        // 失败了也要把文件挪走，否则会每 500ms 重试一次，刷爆日志
        MoveFileExW(tmp, kDonePath, MOVEFILE_REPLACE_EXISTING);
        InterlockedExchange(&g_busy, 0);
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

    // 跑完了再收尾。这时 cmd.exe 已经退出，改名不会失败。
    MoveFileExW(tmp, kDonePath, MOVEFILE_REPLACE_EXISTING);
    InterlockedIncrement(&g_exec_count);
    InterlockedExchange(&g_busy, 0);
    return true;
}

DWORD WINAPI ThreadProc(LPVOID param) {
    (void)param;
    POpen();
    CreateDirectoryW(kDir, nullptr);
    P("命令通道就绪，监听 %ls", kCmdPath);
    P("用法：往 C:\\Xiaoyu\\run.bat 写一个 bat，本线程会以游戏进程的权限执行它");

    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) break;
        const DWORD attr = GetFileAttributesW(kCmdPath);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            RunOnce();
        }
        // 500 -> 100ms：runpy 这条链路希望脚本尽快起来；轮询本身几乎不耗 CPU
        Sleep(100);
    }
    P("命令通道已停止");
    return 0;
}

}  // namespace

void Start() {
    if (InterlockedExchange(&g_started, 1) != 0) return;
    g_thread = CreateThread(nullptr, 0, &ThreadProc, nullptr, 0, nullptr);
}

// 必须等线程真的退出。硬卸载（FreeLibrary）之后如果这个线程还在
// Sleep 里，它醒过来就会跳到已释放的代码段上，直接崩进程。
void Stop() {
    InterlockedExchange(&g_stop, 1);
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

bool Busy() { return InterlockedCompareExchange(&g_busy, 0, 0) != 0; }

int ExecCount() { return (int)g_exec_count; }

}  // namespace bridge
}  // namespace xiaoyu
