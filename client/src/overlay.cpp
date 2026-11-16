// overlay.cpp - Present 钩子的回调：初始化一次，然后每帧驱动 GUI
#include <windows.h>

#include <dxgi.h>

#include "gui.h"
#include "hook.h"
#include "input.h"
#include "log.h"
#include "render.h"

namespace xiaoyu {
namespace {

HWND g_hwnd = nullptr;
bool g_render_ready = false;
bool g_input_ready = false;
bool g_gave_up = false;
int g_fail_count = 0;
ULONGLONG g_last_log = 0;

// 失败也要有熔断：Present 一秒跑 60 次，如果初始化一直失败，
// 不加限制就会变成每秒 60 次重试 + 60 行日志写盘，直接把游戏拖死。
const int kMaxFail = 30;

}  // namespace

// hook.cpp 在 ResizeBuffers 之前调用
void OnSwapChainResize() {
    render::OnResize();
    g_render_ready = false;
}

void OnPresent(IDXGISwapChain* sc) {
    if (sc == nullptr) return;
    if (g_gave_up) return;

    if (!g_input_ready) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow != nullptr) {
            g_hwnd = desc.OutputWindow;
            g_input_ready = input::Attach(g_hwnd);
            LogLine("[overlay] hwnd=%p attach=%d", (void*)g_hwnd, g_input_ready ? 1 : 0);
        }
    }

    if (!g_render_ready) {
        if (!render::BeginFrame(sc)) {
            // 只报前几次，之后就闭嘴，避免刷爆日志
            if (++g_fail_count == 1 || g_fail_count == kMaxFail) {
                LogLine("[overlay] 首帧渲染初始化失败 (%d 次)，停止重试",
                        g_fail_count);
            }
            if (g_fail_count >= kMaxFail) {
                g_gave_up = true;
                LogLine("[overlay] 放弃覆盖层，游戏继续正常运行");
            }
            return;
        }
        render::EndFrame();
        g_render_ready = true;
        g_fail_count = 0;
        gui::Init();
        LogLine("[overlay] first frame ok, %dx%d", render::Width(), render::Height());
        return;
    }

    if (!render::BeginFrame(sc)) return;
    gui::OnFrame();
    render::EndFrame();

    // 每 30 秒报一次存活，方便判断覆盖层还在跑
    ULONGLONG now = GetTickCount64();
    if (now - g_last_log > 30000) {
        g_last_log = now;
        LogLine("[overlay] alive, %dx%d visible=%d", render::Width(), render::Height(),
                gui::Visible() ? 1 : 0);
    }
}

}  // namespace xiaoyu
