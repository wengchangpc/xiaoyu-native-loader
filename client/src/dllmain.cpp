// dllmain.cpp - 注入入口
//
// 流程：
//   DllMain（只能干最轻的活）
//     └─ 起线程
//          └─ 找游戏窗口
//               └─ 建透明置顶覆盖窗口（D3D11 + DirectComposition）
//                    └─ 每帧画 GUI
//
// 注意：这里**不再 hook 任何图形 API**。
//   实测这个 NetEase 客户端是 OpenGL 渲染（窗口类名 OGLES，exe 只导入
//   OPENGL32.dll），进程里没有 DXGI 交换链，Present 钩子永远不会触发。
//   改成独立的透明覆盖窗口之后，我们和游戏的渲染管线彻底解耦 ——
//   它用什么图形 API 都无所谓，加壳也影响不到我们。
//
// 关于"安全模式"：
//   如果 C:\Xiaoyu\safe.txt 存在，本 DLL 只写日志就退出线程，
//   连覆盖窗口都不建。用来把两类问题分开：
//     - safe.txt 存在时游戏仍然崩 → 是注入动作本身触发的（壳的反注入）
//     - safe.txt 存在时游戏正常    → 是我们的覆盖层代码的问题
#include <windows.h>

#include "explorer.h"
#include "log.h"
#include "probe.h"

namespace xiaoyu {
void StartOverlay();
}

namespace {

bool SafeModeEnabled() {
    return GetFileAttributesW(L"C:\\Xiaoyu\\safe.txt") != INVALID_FILE_ATTRIBUTES;
}

DWORD WINAPI BootThread(LPVOID param) {
    (void)param;

    // 第一步就开日志。log.cpp 是纯 Win32 实现，不碰 CRT 流，
    // 这样任何早期崩溃都能在 native.log 里看到停在哪一行。
    xiaoyu::LogInit();
    xiaoyu::LogLine("[boot] 1/4 dll attached (build %s %s)", __DATE__, __TIME__);

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    xiaoyu::LogLine("[boot] 2/4 pid=%lu exe=%s", (unsigned long)GetCurrentProcessId(), exe);

    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    xiaoyu::LogLine("[boot] 3/4 wow64=%d (0 = 64 位)", wow ? 1 : 0);

    if (SafeModeEnabled()) {
        xiaoyu::LogLine("[boot] safe.txt found -- 只写日志，不建覆盖层");
        xiaoyu::LogLine("[boot] 如果游戏此时正常，说明注入没问题，问题在覆盖层");
        return 0;
    }

    xiaoyu::LogLine("[boot] 4/4 启动覆盖层线程");

    // 进程内自诊断（已完成的阶段）：
    //   probe.cpp   走 RTTI 链定位 ClientInstance / LocalPlayer，结果写 probe.log
    //   explorer.cpp 全堆扫实体 + 反查容器，结果写 explorer.log
    //
    // 现在只跑 explorer —— probe 那套信息（RTTI 全表、ClientInstance/LocalPlayer
    // 地址、Level/Dimension 链路）已经拿到并固化进 sdk.h 了，再跑一遍只是多花
    // 30 秒扫一遍堆。要回去看 RTTI 细节时把下面这行放开即可。
    // xiaoyu::probe::Start(3000);

    // ★ 必须放在 StartOverlay() 之前：那个函数会进窗口消息循环、不返回。
    // ★ 延迟 3 秒是为了等进入世界 —— 注入到标题界面时 Actor 还没建。
    xiaoyu::explorer::Start(3000);

    xiaoyu::StartOverlay();
    return 0;
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
