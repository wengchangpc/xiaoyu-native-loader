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
ULONGLONG g_last_log = 0;

}  // namespace

// hook.cpp 在 ResizeBuffers 之前调用
void OnSwapChainResize() {
    render::OnResize();
    g_render_ready = false;
}

void OnPresent(IDXGISwapChain* sc) {
    if (sc == nullptr) return;

    if (!g_input_ready) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow != nullptr) {
            g_hwnd = desc.OutputWindow;
            g_input_ready = input::Attach(g_hwnd);
            LogLine("[overlay] hwnd=%p attach=%d", (void*)g_hwnd, g_input_ready ? 1 : 0);
        }
    }

    if (!g_render_ready) {
        if (!render::BeginFrame(sc)) return;
        render::EndFrame();
        g_render_ready = true;
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
