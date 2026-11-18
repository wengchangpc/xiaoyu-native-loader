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

#include "bridge.h"
#include "explorer.h"
#include "log.h"
#include "memory.h"
#include "probe.h"
#include "rpc.h"
#include "self.h"

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

    // 内存层必须先初始化：sdk.h 里所有访问器都依赖 mem::Base() 拿到主模块基址。
    // 以前这件事是 probe / explorer 干的，两个关掉之后就没人做了 ——
    // mem::Base() 恒为 0，ESP 静默不出框，日志里一个字都没有。
    if (xiaoyu::mem::Init()) {
        xiaoyu::LogLine("[boot] mem 就绪 base=0x%llX size=%.1fMB regions=%d",
                        (unsigned long long)xiaoyu::mem::Base(),
                        (double)xiaoyu::mem::ImageSize() / 1048576.0,
                        (int)xiaoyu::mem::RegionCount());
    } else {
        xiaoyu::LogLine("[boot] !! mem::Init 失败，读内存的模块全部不可用");
    }

    // 文件命令通道：监听 C:\Xiaoyu\run.bat，用游戏进程的权限执行它。
    //
    // 这是整个免打扰方案的地基。游戏是管理员权限跑的，DLL 在里面，所以
    // "以管理员身份跑一个脚本"退化成"往 C:\Xiaoyu 放一个 bat"—— 写普通
    // 文件不需要任何权限，也就不再需要人守着点 UAC。
    xiaoyu::bridge::Start();

    // 进程内命令通道（命名管道 \\.\pipe\xiaoyu_rpc）。
    // 有了它，"读内存 / 枚举实体 / 画框 / 改 ESP 参数"都变成一行命令，
    // 外部脚本连上就能用 —— 不用再往 run.bat 丢文件、等 500ms 轮询、
    // 再去翻日志。调 ESP 对齐这种活，一轮从几秒压到几十毫秒。
    xiaoyu::rpc::Start();

    // 进程内自诊断（历史阶段，现在默认不跑）：
    //   probe.cpp    走 RTTI 链定位 ClientInstance / LocalPlayer，写 probe.log
    //   explorer.cpp 全堆扫实体 + 反查容器，写 explorer.log
    //
    // ★ 两个都关掉了。理由是 bridge 通道打通之后，DLL 只需要做一件事：
    //   以游戏权限执行外部脚本。探索逻辑放在 Python 里改一版只要几秒，
    //   放在 C++ 里改一版要 push + 等 CI + 重新注入（还弹 UAC），
    //   效率差一个数量级。
    //   更要紧的是 explorer 的全堆扫描会把游戏扫崩 —— 累计 40 秒左右的
    //   遍历就会被反作弊收掉，而它一崩 bridge 也跟着没了。
    //   需要时把下面任何一行放开即可。
    // xiaoyu::probe::Start(3000);
    // xiaoyu::explorer::Start(3000);

    xiaoyu::StartOverlay();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // 记下自己是谁：卸载（FreeLibraryAndExitThread）要用。
        // 这一步只是存一个指针，可以放在 loader lock 里。
        xiaoyu::self::SetModule(hModule);
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
