// dllmain.cpp - 注入入口：等解释器就绪，然后把内嵌脚本丢进去跑
#include <windows.h>

#include "log.h"
#include "python_host.h"

// 由 tools/embed.py 生成：python/bootstrap.py 的字节数组
#include "embedded_bootstrap.h"

static DWORD WINAPI BootThread(LPVOID param) {
    (void)param;
    LogLine("");
    LogLine("================ xiaoyu loader attach ================");
    LogLine("[sys] pid=%lu log=%s", (unsigned long)GetCurrentProcessId(), LogPath());

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    LogLine("[sys] exe=%s", exe);

    BOOL is_wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &is_wow64);
    LogLine("[sys] wow64=%d (0 = 本进程是 64 位)", is_wow64 ? 1 : 0);

    PyApi api;
    // 解释器可能还没加载：最多等 180 秒
    const int kMaxWaitMs = 180000;
    const int kStepMs = 250;
    int waited = 0;
    bool resolved = false;
    while (waited < kMaxWaitMs) {
        if (PyResolve(&api)) {
            resolved = true;
            break;
        }
        Sleep(kStepMs);
        waited += kStepMs;
    }

    if (!resolved) {
        LogLine("[py] 180 秒内没找到 python*.dll —— 判定注入时机太早或不是 ModSDK 进程");
        DumpLoadedModules();
        return 0;
    }

    LogLine("[py] 就绪，等待时间 %d ms", waited);
    LogLine("[py] version=%s", PyVersion(&api).c_str());

    // 解释器已加载但可能还没初始化（引擎要等进世界）
    int init_waited = 0;
    while (!PyIsReady(&api) && init_waited < kMaxWaitMs) {
        Sleep(kStepMs);
        init_waited += kStepMs;
    }
    if (!PyIsReady(&api)) {
        LogLine("[py] 解释器已加载但 180 秒内未初始化 —— 先进一个存档再注入");
        DumpLoadedModules();
        return 0;
    }
    LogLine("[py] 解释器已初始化（等待 %d ms）", init_waited);

    DumpLoadedModules();

    // 内嵌脚本可能不含结尾 0，按长度构造
    std::string code(reinterpret_cast<const char*>(kBootstrap),
                     (size_t)kBootstrapLen);
    LogLine("[run] 脚本 %d 字节", (int)code.size());
    int rc = PyRunScript(&api, code.c_str());
    LogLine("[run] PyRun 返回 %d（0 = 脚本正常结束）", rc);
    LogLine("================ attach done ================");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // DllMain 里不能干重活（loader lock），另起线程
        HANDLE th = CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr);
        if (th != nullptr) {
            CloseHandle(th);
        }
    }
    return TRUE;
}
