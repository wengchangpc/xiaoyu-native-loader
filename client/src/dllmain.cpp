// dllmain.cpp - 注入入口
//
// 流程：
//   DllMain（只能干最轻的活）
//     └─ 起线程
//          └─ 建一个临时 D3D11 交换链拿到 vtable，挂 Present
//               └─ 游戏第一次 Present 时：拿 HWND、子类化窗口、初始化 D2D
//                    └─ 每帧画覆盖层
//
// 关于"安全模式"：
//   如果 C:\Xiaoyu\safe.txt 存在，本 DLL 只写日志、不挂 Present 就直接退出线程。
//   用来把两类问题分开：
//     - safe.txt 存在时游戏仍然崩 → 是注入动作本身（大概率是壳的反注入）触发的
//     - safe.txt 存在时游戏正常    → 是我们的 hook / D2D 代码的问题
#include <windows.h>

#include <dxgi.h>

#include "hook.h"
#include "log.h"

namespace xiaoyu {
void OnPresent(IDXGISwapChain* sc);
}

namespace {

// 把可能抛 SEH 的调用包起来。
// 注意：函数里不能有需要析构的 C++ 对象，否则 MSVC 会报 C2712。
bool TryInstall(unsigned long* seh_code) {
    __try {
        return xiaoyu::hook::Install(&xiaoyu::OnPresent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *seh_code = GetExceptionCode();
        return false;
    }
}

bool SafeModeEnabled() {
    return GetFileAttributesW(L"C:\\Xiaoyu\\safe.txt") != INVALID_FILE_ATTRIBUTES;
}

DWORD WINAPI BootThread(LPVOID param) {
    (void)param;

    // 第一步就开日志。log.cpp 是纯 Win32 实现，不碰 CRT 流，
    // 这样任何早期崩溃都能在 native.log 里看到停在哪一行。
    xiaoyu::LogInit();
    xiaoyu::LogLine("[boot] 1/6 dll attached (build %s %s)", __DATE__, __TIME__);

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    xiaoyu::LogLine("[boot] 2/6 pid=%lu exe=%s", (unsigned long)GetCurrentProcessId(), exe);

    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    xiaoyu::LogLine("[boot] 3/6 wow64=%d (0 = 64 位)", wow ? 1 : 0);

    if (SafeModeEnabled()) {
        xiaoyu::LogLine("[boot] safe.txt found -- 不挂 Present，线程直接退出");
        xiaoyu::LogLine("[boot] 如果游戏此时正常，说明注入没问题，问题在覆盖层");
        return 0;
    }

    // 等一下：有些保护会在进程刚起来时扫模块，太急容易被盯上
    Sleep(1500);
    xiaoyu::LogLine("[boot] 4/6 开始挂 Present");

    for (int attempt = 0; attempt < 5; ++attempt) {
        unsigned long seh = 0;
        if (TryInstall(&seh)) {
            xiaoyu::LogLine("[hook] ok on attempt %d", attempt + 1);
            return 0;
        }
        if (seh != 0) {
            xiaoyu::LogLine("[hook] attempt %d 抛异常 code=0x%08lX", attempt + 1, seh);
        } else {
            xiaoyu::LogLine("[hook] attempt %d failed, retrying", attempt + 1);
        }
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
        if (th != nullptr) {
            CloseHandle(th);
        } else {
            // 连线程都起不来就别指望后面了，但至少留个痕
            xiaoyu::LogInit();
            xiaoyu::LogLine("[boot] CreateThread failed err=%lu",
                            (unsigned long)GetLastError());
        }
    }
    return TRUE;
}
