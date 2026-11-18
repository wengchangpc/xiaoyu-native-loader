// self.cpp - 见 self.h
//
// 卸载为什么要单开一个线程：
//   触发卸载的调用点几乎都在"别人"的线程上 —— 管道命令线程、
//   或者 DllMain 之后的引导线程。在这些线程上直接 FreeLibrary 等于
//   把脚下的地板抽掉。所以统一改成：记下请求 → 起一个一次性线程 →
//   由它去停所有子系统、最后 FreeLibraryAndExitThread 把**自己**送走。
#include "self.h"

#include <windows.h>

#include "bridge.h"
#include "input.h"
#include "log.h"
#include "rpc.h"

namespace xiaoyu {

// overlay.cpp 里导出：停覆盖层并等它退干净
void StopOverlay();

namespace self {
namespace {

HMODULE g_mod = nullptr;
volatile LONG g_req = 0;    // 已经请求过卸载（幂等标记）
volatile LONG g_hard = 0;   // 1 = 尝试真的 FreeLibrary

DWORD WINAPI UnloadThread(LPVOID param) {
    (void)param;

    LogLine("[self] 开始卸载（hard=%d）", (int)g_hard);

    // 顺序有讲究：先断开**外部**入口，再停内部线程。
    // 反过来的话，刚停完 bridge 就可能被下一条管道命令又叫起来。
    rpc::Stop();       // 关管道，之后没人能再命令我们
    bridge::Stop();    // 停 run.bat 轮询（内部会等线程退出）
    StopOverlay();     // 覆盖层收尾：光标计数 / 钩子 / 窗口 / D2D
    input::ShutdownHooks();

    if (g_hard == 0) {
        LogLine("[self] 软卸载完成：线程全部停下，DLL 驻留但不再做任何事");
        return 0;
    }

    // ── 硬卸载的三个前置条件 ────────────────────────────────────
    // 1) 命令通道没在跑子进程。跑着的话，子进程结束时控制权会回到
    //    已经释放掉的代码段 —— 直接崩进程。
    if (bridge::Busy()) {
        LogLine("[self] bridge 正在跑子进程，退化为软卸载（重进游戏才能再注入）");
        return 0;
    }
    // 2) 得知道自己是谁。
    const HMODULE m = g_mod;
    if (m == nullptr) {
        LogLine("[self] 没拿到 hModule，无法硬卸载");
        return 0;
    }
    // 3) 日志先刷盘 —— 下面这一句之后就没有代码可以执行了。
    LogLine("[self] 硬卸载：FreeLibraryAndExitThread(0x%llX)",
            (unsigned long long)(uintptr_t)m);
    Sleep(120);

    FreeLibraryAndExitThread(m, 0);
    return 0;   // 走不到
}

}  // namespace

void SetModule(HMODULE m) { g_mod = m; }
HMODULE Module() { return g_mod; }

bool RequestUnload(bool hard) {
    if (InterlockedExchange(&g_req, 1) != 0) return false;
    if (hard) InterlockedExchange(&g_hard, 1);

    HANDLE th = CreateThread(nullptr, 0, &UnloadThread, nullptr, 0, nullptr);
    if (th == nullptr) {
        LogLine("[self] CreateThread 失败 err=%lu，卸载取消",
                (unsigned long)GetLastError());
        InterlockedExchange(&g_req, 0);
        return false;
    }
    CloseHandle(th);
    return true;
}

bool UnloadRequested() { return InterlockedCompareExchange(&g_req, 0, 0) != 0; }

}  // namespace self
}  // namespace xiaoyu
