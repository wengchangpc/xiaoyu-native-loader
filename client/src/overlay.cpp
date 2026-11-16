// overlay.cpp - 独立透明置顶覆盖窗口
//
// 为什么不再 hook 游戏的交换链：
//   这个 NetEase 客户端是 OpenGL 渲染（窗口类名 OGLES，exe 只导入 OPENGL32.dll），
//   进程里没有任何 DXGI 交换链，所以 IDXGISwapChain::Present 永远不被调用 ——
//   实测日志停在 "[hook] ok" 之后就再没动静，游戏不崩但面板也不出来。
//
//   改成自建一个 WS_EX_NOREDIRECTIONBITMAP 的置顶透明窗口，用
//   DirectComposition 把自己的合成交换链贴到 DWM 上。好处是：
//     1. 完全不碰游戏的内存和代码，加壳对我们没有影响；
//     2. 不依赖游戏的图形 API，将来它换后端也不用改；
//     3. 现有 D2D 绘制代码和 gui.cpp 一行都不用动。
//
// 点击穿透（这条是必须的，否则 HUD 一显示游戏就点不动了）：
//   面板收起时给窗口加上 WS_EX_TRANSPARENT —— 分层窗口 + WS_EX_TRANSPARENT
//   是覆盖层里用了十几年的标准穿透组合，行为确定。面板打开时去掉它，
//   相当于一个模态菜单。
//
//   踩过的坑：之前还额外调了 EnableWindow(FALSE) 想"双保险"，结果被禁用的
//   窗口虽然不处理鼠标，却也不会把点击转给下面的窗口，而是直接丢弃 ——
//   表现就是游戏里左键挖不了方块。所以这里绝对不能再禁用窗口。
//
// 键盘不靠窗口消息：统一用 GetAsyncKeyState 轮询（见 input.cpp），
// 这样游戏抢着焦点也能读到按键。
#include <windows.h>
#include <objbase.h>   // CoInitializeEx
#include <windowsx.h>

#include "config.h"
#include "gui.h"
#include "input.h"
#include "log.h"
#include "render.h"

namespace xiaoyu {
namespace {

const wchar_t* kClassName = L"XiaoyuOverlayWnd";

HWND g_game = nullptr;     // 游戏主窗口
HWND g_overlay = nullptr;  // 我们的覆盖窗口

int g_cached_x = 0, g_cached_y = 0, g_cached_w = 0, g_cached_h = 0;
bool g_visible = false;
bool g_interactive = false;
bool g_running = false;
ULONGLONG g_unlock_check_at = 0;  // 合成解锁键之后多久去确认一次效果
int g_unlock_vk = 0;              // 这次合成的是哪个键
int g_unlock_stage = 0;           // 0=空闲 1=已 SendInput 2=已投递窗口消息

struct FindCtx {
    DWORD pid;
    HWND best;
    LONG best_area;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);

    if (hwnd == g_overlay) return TRUE;   // 别把自家窗口认成游戏

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    // 只认游戏窗口类。之前只按面积挑最大的，结果覆盖窗口本身
    // （1215x702，比游戏客户区还大一圈）会因为面积胜出被选中，
    // 日志里出现过 "[overlay] 游戏窗口 hwnd=...09308EA" 这种自指。
    {
        wchar_t cls[64] = {0};
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, kClassName) == 0) return TRUE;
    }

    // 只要顶层、无 owner 的窗口
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    LONG w = rc.right - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w < 200 || h < 150) return TRUE;

    LONG area = w * h;
    if (area > ctx->best_area) {
        ctx->best_area = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

// 找同进程里面积最大的可见顶层窗口
HWND FindGameWindow() {
    FindCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.best = nullptr;
    ctx.best_area = 0;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// ── 滚轮 ────────────────────────────────────────────────────────
// WM_MOUSEWHEEL 只发给"有焦点"的窗口，而我们是 WS_EX_NOACTIVATE，
// 焦点永远在游戏那边，所以正常途径收不到滚轮（实测面板滚不动就是这个原因）。
//
// 改用 Raw Input 的 RIDEV_INPUTSINK：窗口不在前台也能收到 WM_INPUT。
// 只在面板打开期间注册 —— INPUTSINK 会把每一条鼠标消息都投过来，
// 高刷新率鼠标一秒上千条，没必要一直挂着。
void RegisterWheelSink(bool on) {
    if (g_overlay == nullptr) return;
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;   // Generic Desktop Controls
    rid.usUsage = 0x02;       // Mouse
    rid.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget = on ? g_overlay : nullptr;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        LogLine("[overlay] RegisterRawInputDevices(%d) failed err=%lu", on ? 1 : 0,
                (unsigned long)GetLastError());
    } else if (on) {
        LogLine("[overlay] 已挂上 Raw Input 滚轮接收");
    }
}

// 让游戏交还鼠标。
// 游戏自己把光标锁在窗口里，外部 ClipCursor(nullptr) 会被它下一帧重新设回去
// （实测每帧解一次也没用）。用户手动按 Alt 能解锁，说明这是游戏自己的绑定，
// 那就替用户按一下 —— 打开面板时按一次，关掉时再按一次恢复，正好对称。
//
// 合成有两条路：
//   A. SendInput —— 走系统输入队列，最"真"，但注入事件带 LLKHF_INJECTED 标记，
//      有些客户端/反作弊会直接丢掉；
//   B. PostMessage WM_SYSKEYDOWN —— 直接投给游戏自己的 WndProc，不经过
//      SendInput 的标记检查，代价是只对"从窗口消息读键盘"的客户端有效。
// 先走 A，400ms 后看光标是否还被裁剪；还锁着就退到 B 再试一次。
void PostKeyToGame(int vk, bool down) {
    if (g_game == nullptr || !IsWindow(g_game)) return;
    const bool menu_key = (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
    const UINT msg = down ? (menu_key ? WM_SYSKEYDOWN : WM_KEYDOWN)
                          : (menu_key ? WM_SYSKEYUP : WM_KEYUP);
    LPARAM lp = 1;
    lp |= (LPARAM)(MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC) & 0xFF) << 16;
    if (!down) lp |= (LPARAM)3 << 30;   // 重复 1 次 + 前一个状态为按下
    PostMessageW(g_game, msg, (WPARAM)vk, lp);
}

// verify：只有"打开面板"这次需要回头看效果。关面板时按完就把鼠标还给游戏了，
// 这时候去查裁剪状态只会得到 1（游戏重新抓住），日志会误导人。
void ToggleGameCursorRelease(const char* why, bool verify) {
    int vk = cfg::UnlockKey();
    LogLine("[overlay] 合成解锁键 vk=0x%02X（%s），当前是否被裁剪=%d", vk, why,
            input::IsCursorClipped() ? 1 : 0);
    input::SendKeyTap(vk);
    if (!verify) {
        g_unlock_stage = 0;
        g_unlock_check_at = 0;
        return;
    }
    g_unlock_vk = vk;
    g_unlock_stage = 1;
    g_unlock_check_at = GetTickCount64() + 400;
}

// 面板可见性变化时切换命中测试模式
void UpdateInputMode() {
    if (g_overlay == nullptr) return;
    bool want = gui::Visible();
    if (want == g_interactive) return;
    g_interactive = want;

    // 注意：SDK 里没有 GWLP_EXSTYLE 这个常量。
    // GWLP_* 那一套只覆盖指针大小的项（WNDPROC / HINSTANCE / HWNDPARENT /
    // USERDATA / ID），style 和 exstyle 永远是 32 位的 GWL_*。
    LONG ex = GetWindowLongW(g_overlay, GWL_EXSTYLE);
    if (want) {
        ex &= ~(LONG)WS_EX_TRANSPARENT;
    } else {
        ex |= (LONG)WS_EX_TRANSPARENT;
    }
    SetWindowLongW(g_overlay, GWL_EXSTYLE, ex);

    SetWindowPos(g_overlay, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);

    if (want) {
        // 面板打开时解除游戏的鼠标裁剪，免得光标被困在窗口里
        ClipCursor(nullptr);
        // 替用户按一下游戏自己的"释放鼠标"键（内部会把校验时间点排好）
        if (cfg::AutoUnlock()) ToggleGameCursorRelease("打开面板", true);
        RegisterWheelSink(true);
        // 顺便让渲染层在下一帧做一次像素自检，把面板的实际绘制范围写进日志
        render::RequestSelfCheck();
    } else {
        RegisterWheelSink(false);
        // 关掉面板时把鼠标交回游戏，不然游戏里会一直没有视角控制
        if (cfg::AutoUnlock()) ToggleGameCursorRelease("关闭面板", false);
    }
    LogLine("[overlay] 输入模式 -> %s", want ? "接管鼠标" : "点击穿透");
}

// 覆盖窗口必须和游戏客户区严格对齐
void FollowGameWindow() {
    if (g_overlay == nullptr) return;

    if (g_game == nullptr || !IsWindow(g_game)) {
        g_game = FindGameWindow();
        if (g_game == nullptr) return;
        LogLine("[overlay] 游戏窗口 hwnd=%p", (void*)g_game);
    }

    // 游戏最小化 / 不在前台时把覆盖窗口收起来，免得盖在别人窗口上
    bool active = !IsIconic(g_game) && GetForegroundWindow() == g_game;
    if (!active) {
        if (g_visible) {
            ShowWindow(g_overlay, SW_HIDE);
            g_visible = false;
        }
        return;
    }

    RECT cr;
    if (!GetClientRect(g_game, &cr)) return;
    POINT tl = {0, 0};
    if (!ClientToScreen(g_game, &tl)) return;

    int w = (int)(cr.right - cr.left);
    int h = (int)(cr.bottom - cr.top);
    if (w <= 0 || h <= 0) return;

    bool moved = (tl.x != g_cached_x || tl.y != g_cached_y);
    bool resized = (w != g_cached_w || h != g_cached_h);
    if (moved || resized) {
        SetWindowPos(g_overlay, HWND_TOPMOST, tl.x, tl.y, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        g_cached_x = tl.x;
        g_cached_y = tl.y;
        g_cached_w = w;
        g_cached_h = h;
        if (resized) {
            render::Resize(w, h);
            LogLine("[overlay] 尺寸对齐到 %dx%d @ (%d,%d)", w, h, tl.x, tl.y);
        }
    }

    if (!g_visible) {
        ShowWindow(g_overlay, SW_SHOWNA);  // 显示但不抢焦点
        g_visible = true;
    }
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            // 面板开着才接管鼠标，否则整块区域让给下面的窗口
            return g_interactive ? HTCLIENT : HTTRANSPARENT;

        case WM_MOUSEWHEEL:
            input::AddWheel((float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA);
            return 0;

        case WM_INPUT: {
            // Raw Input 送来的滚轮。面板打开时我们挂了 RIDEV_INPUTSINK，
            // 所以窗口没焦点也能收到。
            RAWINPUT raw = {};
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                if (raw.header.dwType == RIM_TYPEMOUSE &&
                    (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
                    short delta = (short)raw.data.mouse.usButtonData;
                    input::AddWheel((float)delta / (float)WHEEL_DELTA);
                }
            }
            break;  // 不吞，继续交给 DefWindowProc
        }

        case WM_ERASEBKGND:
            return 1;  // 内容全由 DComp 合成，别让 GDI 刷一层底色

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // 点面板不抢焦点，游戏不会因此暂停

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateOverlayWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hbrBackground = nullptr;  // 不要背景刷
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassExW(&wc) == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LogLine("[overlay] RegisterClass failed err=%lu", (unsigned long)err);
            return false;
        }
    }

    // 分层窗口：内容由 render.cpp 的 UpdateLayeredWindow 提交，
    // 所以这里要 WS_EX_LAYERED，且绝不能用 WS_EX_NOREDIRECTIONBITMAP（两者互斥）。
    // TOOLWINDOW 让它不进任务栏和 Alt+Tab，NOACTIVATE 保证不抢焦点。
    // 初始带 TRANSPARENT（点击穿透），面板打开后由 UpdateInputMode 去掉。
    DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
               WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;

    g_overlay = CreateWindowExW(ex, kClassName, L"xiaoyu overlay", WS_POPUP,
                                0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    if (g_overlay == nullptr) {
        LogLine("[overlay] CreateWindowEx failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    LogLine("[overlay] 覆盖窗口已创建 hwnd=%p", (void*)g_overlay);
    return true;
}

void RenderOnce() {
    if (!render::BeginFrame()) return;
    gui::OnFrame();
    render::EndFrame();
}

int OverlayMain() {
    // 等游戏窗口出现（注入时机可能比窗口创建还早）
    for (int i = 0; i < 150 && g_game == nullptr; ++i) {
        g_game = FindGameWindow();
        if (g_game != nullptr) break;
        Sleep(200);
    }
    if (g_game == nullptr) {
        LogLine("[overlay] 等不到游戏窗口（30 秒），放弃");
        return 1;
    }

    wchar_t cls[64] = {0};
    GetClassNameW(g_game, cls, 64);
    LogLine("[overlay] 游戏窗口 hwnd=%p class=%ls", (void*)g_game, cls);

    if (!CreateOverlayWindow()) return 1;
    if (!input::Attach(g_overlay)) return 1;

    // 先把窗口摆到游戏客户区上，再建交换链（尺寸必须一开始就对）
    RECT cr = {0, 0, 0, 0};
    GetClientRect(g_game, &cr);
    POINT tl = {0, 0};
    ClientToScreen(g_game, &tl);
    int w = (int)(cr.right - cr.left);
    int h = (int)(cr.bottom - cr.top);
    if (w <= 0 || h <= 0) { w = 1280; h = 720; }

    SetWindowPos(g_overlay, HWND_TOPMOST, tl.x, tl.y, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_cached_x = tl.x;
    g_cached_y = tl.y;
    g_cached_w = w;
    g_cached_h = h;

    if (!render::Init(g_overlay, w, h)) {
        LogLine("[overlay] render::Init 失败，覆盖层不可用");
        return 1;
    }

    gui::Init();
    ShowWindow(g_overlay, SW_SHOWNA);
    g_visible = true;
    LogLine("[overlay] 就绪 %dx%d @ (%d,%d)，按右 Shift 开关面板", w, h, tl.x, tl.y);

    g_running = true;
    ULONGLONG next_frame = GetTickCount64();
    ULONGLONG last_report = GetTickCount64();

    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        FollowGameWindow();

        if (g_visible) {
            // 注意：不要在这里调 input::NewFrame()。
            // gui::OnFrame() 内部已经调过了，一个帧里调两次会把 pressed/released
            // 的边沿状态清掉，点击就丢了。
            UpdateInputMode();
            RenderOnce();

            // 合成解锁键之后确认一次效果，方便判断这个绑定到底管不管用
            if (g_unlock_check_at != 0 && GetTickCount64() >= g_unlock_check_at) {
                g_unlock_check_at = 0;
                const bool clipped = input::IsCursorClipped();
                if (g_unlock_stage == 1) {
                    LogLine("[overlay] 解锁检查 1/2（SendInput）：cursor_clipped=%d",
                            clipped ? 1 : 0);
                    if (!clipped) {
                        g_unlock_stage = 0;
                    } else {
                        // SendInput 没生效：要么被注入标记过滤，要么游戏根本不看
                        // 系统输入队列。换成直接往游戏窗口投键盘消息再试一次。
                        LogLine("[overlay] SendInput 无效，改用 PostMessage 投递 vk=0x%02X",
                                g_unlock_vk);
                        PostKeyToGame(g_unlock_vk, true);
                        PostKeyToGame(g_unlock_vk, false);
                        g_unlock_stage = 2;
                        g_unlock_check_at = GetTickCount64() + 400;
                    }
                } else if (g_unlock_stage == 2) {
                    g_unlock_stage = 0;
                    LogLine("[overlay] 解锁检查 2/2（PostMessage）：cursor_clipped=%d",
                            clipped ? 1 : 0);
                    if (clipped) {
                        LogLine("[overlay] 两条路都没解开光标。可行做法：把 "
                                "C:\\Xiaoyu\\native_config.txt 里的 unlockkey 改成游戏实际的"
                                "释放键（vkid 见同目录键码表），或 unlock=0 继续手动按");
                    }
                } else {
                    LogLine("[overlay] 解锁检查：cursor_clipped=%d", clipped ? 1 : 0);
                }
            }

            ULONGLONG now = GetTickCount64();
            if (now - last_report > 30000) {
                last_report = now;
                LogLine("[overlay] alive, %dx%d gui=%d mouse=(%.0f,%.0f)",
                        render::Width(), render::Height(), gui::Visible() ? 1 : 0,
                        input::X(), input::Y());
            }

            // 面板开着按 60fps 刷，收起时 20fps 省开销
            next_frame += gui::Visible() ? 16 : 50;
            now = GetTickCount64();
            if (next_frame > now) {
                DWORD wait = (DWORD)(next_frame - now);
                Sleep(wait > 50 ? 50 : wait);
            } else {
                next_frame = now;
            }
        } else {
            Sleep(30);
        }
    }

    render::Shutdown();
    if (g_overlay != nullptr) DestroyWindow(g_overlay);
    g_overlay = nullptr;
    LogLine("[overlay] 线程退出");
    return 0;
}

DWORD WINAPI OverlayThread(LPVOID param) {
    (void)param;

    // DirectComposition 要求线程先初始化 COM。
    // 用 STA：我们本来就在这个线程跑消息循环，公寓线程模型正合适。
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
        // 已经按别的模型初始化过也能继续，只是记一笔
        LogLine("[overlay] CoInitializeEx hr=0x%08lX（继续尝试）", (unsigned long)co);
    }

    int rc = OverlayMain();

    if (SUCCEEDED(co)) CoUninitialize();
    return (DWORD)rc;
}

}  // namespace

void StartOverlay() {
    HANDLE th = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (th != nullptr) {
        CloseHandle(th);
    } else {
        LogLine("[overlay] CreateThread failed err=%lu", (unsigned long)GetLastError());
    }
}

}  // namespace xiaoyu
