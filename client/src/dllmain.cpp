// dllmain.cpp - 注入入口
//
// 流程：
//   DllMain（只能干最轻的活）
//     └─ 起线程
//          └─ 建一个临时 D3D11 交换链拿到 vtable，挂 Present
//               └─ 游戏第一次 Present 时：拿 HWND、子类化窗口、初始化 D2D
//                    └─ 每帧画覆盖层
#include <windows.h>

#include <dxgi.h>

#include "hook.h"
#include "log.h"

namespace xiaoyu {
void OnPresent(IDXGISwapChain* sc);
}

namespace {

DWORD WINAPI BootThread(LPVOID param) {
    (void)param;
    xiaoyu::LogInit();

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    xiaoyu::LogLine("[sys] pid=%lu exe=%s", (unsigned long)GetCurrentProcessId(), exe);

    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    xiaoyu::LogLine("[sys] wow64=%d (0 = 64 位)", wow ? 1 : 0);

    // 等一下：有些保护会在进程刚起来时扫模块，太急容易被盯上
    Sleep(1500);

    for (int attempt = 0; attempt < 5; ++attempt) {
        if (xiaoyu::hook::Install(&xiaoyu::OnPresent)) {
            xiaoyu::LogLine("[hook] ok on attempt %d", attempt + 1);
            return 0;
        }
        xiaoyu::LogLine("[hook] attempt %d failed, retrying", attempt + 1);
        Sleep(1000);
    }

    xiaoyu::LogLine("[hook] gave up -- 覆盖层没有挂上");
    return 1;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // DllMain 在 loader lock 里，不能建 D3D 设备
        HANDLE th = CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr);
        if (th != nullptr) CloseHandle(th);
    }
    return TRUE;
}
