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
ULONGLONG g_release_check_at = 0;  // 关面板交还焦点之后，过一会儿确认游戏有没有抓回鼠标

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

// ── 鼠标 Raw Input 接管 ─────────────────────────────────────────
// 走过的弯路，别再踩：
//   1) RIDEV_INPUTSINK + RIDEV_REMOVE 收滚轮 —— REMOVE 把**游戏自己那份**
//      鼠标注册一起删了，游戏再也读不到相对位移，视角永久锁死。
//   2) 抢前台让游戏自己放开鼠标 —— 基岩版在失焦时会弹出自己的 Esc 菜单，
//      那个菜单会和 GUI 抢置顶，而且关掉 GUI 后游戏还停在菜单里。
//
// 现在这条路：面板打开时把鼠标的 Raw Input **接到我们自己窗口**上
// （同一 usage 的注册是进程内唯一的，注册我们这份就等于顶掉游戏那份），
// 于是游戏收不到位移、视角冻结，但它不失焦、不弹菜单、照常渲染；
// 面板关闭时把注册还给游戏窗口，视角立刻恢复。
//
// 代价：游戏不再维护光标，系统光标停在原地。所以坐标改用相对位移累加
// （input::SetRelativeMode），由我们画一个软件光标。
bool g_raw_hooked = false;      // 只在状态真正切换时打日志
ULONGLONG g_raw_reassert_at = 0;

void HookMouseToOverlay(bool on) {
    if (g_overlay == nullptr) return;
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;   // Generic Desktop Controls
    rid.usUsage = 0x02;       // Mouse
    if (on) {
        rid.dwFlags = RIDEV_INPUTSINK;   // 窗口不在前台也要收
        rid.hwndTarget = g_overlay;
    } else {
        // 还给游戏。游戏自己是"仅前台接收"，前台状态下行为一致。
        // 找不到游戏窗口就干脆不动 —— 传 nullptr 会注册失败，
        // 反而可能把已有的注册搞成半残状态。
        if (g_game == nullptr || !IsWindow(g_game)) g_game = FindGameWindow();
        if (g_game == nullptr) return;
        rid.dwFlags = 0;
        rid.hwndTarget = g_game;
    }
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        LogLine("[overlay] 鼠标 Raw Input 切换失败(%d) err=%lu", on ? 1 : 0,
                (unsigned long)GetLastError());
        return;
    }
    // 重复调用只重注册、不改模式：有些引擎检测到鼠标设备变化会自己抢回去，
    // 面板打开期间每秒重申一次，确保冻结是持续的。
    if (g_raw_hooked != on) {
        g_raw_hooked = on;
        input::SetRelativeMode(on);
        LogLine("[overlay] 鼠标 Raw Input -> %s", on ? "覆盖层（游戏视角冻结）" : "游戏");
    }
}

// 兜底路径：合成游戏自己的"释放鼠标"键。
// 默认关闭（cfg::AutoUnlock() 为假）。只有 focus=0 且这条路的键位确实
// 对得上时才用 —— 之前连按两次会把游戏卡在光标模式里。
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

// 把前台/焦点强行交给 target。
// 关键前提：覆盖窗口是注入到游戏进程里创建的，和游戏窗口同属一个进程，
// 所以这是进程内的焦点切换，不受 SetForegroundWindow 的跨进程限制。
// 跨线程设焦点必须先 AttachThreadInput —— 焦点是"线程级"的概念。
void ForceForeground(HWND target) {
    if (target == nullptr || !IsWindow(target)) return;

    HWND fg = GetForegroundWindow();
    const DWORD my_tid = GetCurrentThreadId();
    const DWORD fg_tid = (fg != nullptr) ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD tg_tid = GetWindowThreadProcessId(target, nullptr);

    // 两段 attach：前台线程（否则 SetForegroundWindow 可能被拒）
    // 和 target 线程（否则 SetFocus 跨线程无效）。
    bool a_fg = (fg_tid != 0 && fg_tid != my_tid) &&
                AttachThreadInput(my_tid, fg_tid, TRUE) != FALSE;
    bool a_tg = (tg_tid != 0 && tg_tid != my_tid && tg_tid != fg_tid) &&
                AttachThreadInput(my_tid, tg_tid, TRUE) != FALSE;

    if (!IsWindowVisible(target)) ShowWindow(target, SW_SHOWNA);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetActiveWindow(target);
    SetFocus(target);

    if (a_tg) AttachThreadInput(my_tid, tg_tid, FALSE);
    if (a_fg) AttachThreadInput(my_tid, fg_tid, FALSE);
}

// 打开面板：抢前台。游戏收到 WM_ACTIVATE(WA_INACTIVE) 后会自己放开鼠标捕获，
// 这是它自己的逻辑，不依赖任何键位绑定，也不会被反作弊的注入标记拦截。
//
// 为什么不再一上来就合成按键：实测两次 Alt 之后游戏停在"光标模式"里出不来，
// 关掉面板也没法恢复视角。抢焦点是对称的 —— 焦点给谁，鼠标捕获就跟着谁。
RECT g_game_rect_before = {};
ULONGLONG g_rect_check_at = 0;

void TakeForeground() {
    g_game_rect_before = RECT{};
    if (g_game != nullptr && IsWindow(g_game)) {
        GetWindowRect(g_game, &g_game_rect_before);
    }
    g_rect_check_at = GetTickCount64() + 600;
    ForceForeground(g_overlay);
    LogLine("[overlay] 接管前台 fg=%p overlay=%p clipped=%d", (void*)GetForegroundWindow(),
            (void*)g_overlay, input::IsCursorClipped() ? 1 : 0);
}

// 关闭面板：把前台还给游戏窗口，并顺手把光标放回客户区中心 ——
// 游戏重新捕获鼠标时一般也是从中心开始算相对位移，先摆正少一次视角跳变。
void ReleaseToGame() {
    if (g_game == nullptr || !IsWindow(g_game)) return;
    ForceForeground(g_game);

    // 有些客户端不看 SetForegroundWindow 的结果，只认 WM_ACTIVATE。
    // 补发一条 WA_ACTIVE，让它的输入循环重新跑"抢回鼠标"那段逻辑。
    PostMessageW(g_game, WM_ACTIVATEAPP, TRUE, (LPARAM)GetCurrentThreadId());
    PostMessageW(g_game, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);

    RECT rc = {};
    if (GetClientRect(g_game, &rc)) {
        POINT c;
        c.x = (rc.right - rc.left) / 2;
        c.y = (rc.bottom - rc.top) / 2;
        ClientToScreen(g_game, &c);
        SetCursorPos(c.x, c.y);
    }
    g_release_check_at = GetTickCount64() + 400;
    LogLine("[overlay] 前台已交还游戏 hwnd=%p cursor_visible=%d", (void*)g_game,
            input::CursorVisible() ? 1 : 0);
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
    // 只切 TRANSPARENT。NOACTIVATE 必须一直保留 —— 一旦点击覆盖层能激活它，
    // 游戏就会失焦，而基岩版失焦时会弹出自己的 Esc 菜单，那个菜单会和
    // GUI 抢置顶，关掉 GUI 后游戏也还停在菜单里出不来。
    LONG_PTR ex = GetWindowLongPtrW(g_overlay, GWL_EXSTYLE);
    if (want) {
        ex &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    } else {
        ex |= (LONG_PTR)WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, ex);

    SetWindowPos(g_overlay, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);

    if (want) {
        // 不再抢前台。失焦会让基岩版弹出自己的 Esc 菜单，
        // 那个菜单既会跟 GUI 抢置顶，又会在关掉 GUI 之后继续暂停游戏。
        // 改成接管鼠标的 Raw Input：游戏收不到位移就等于视角冻结，
        // 但它始终是前台窗口、始终在跑。
        if (cfg::TakeFocus()) TakeForeground();
        if (cfg::AutoUnlock()) ToggleGameCursorRelease("打开面板", true);
        HookMouseToOverlay(true);
        // 顺便让渲染层在下一帧做一次像素自检，把面板的实际绘制范围写进日志
        render::RequestSelfCheck();
    } else {
        HookMouseToOverlay(false);
        // 把前台还给游戏，视角控制立刻恢复
        if (cfg::TakeFocus()) ReleaseToGame();
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
    // 最小化时客户区会变成 0x0，这时候去 Resize 交换链/重建 DIB 只会崩，
    // 直接跳过，等窗口恢复再跟。
    if (w < 16 || h < 16) return;

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
            // 面板打开期间鼠标注册在我们身上，位移和滚轮都从这里来
            RAWINPUT raw = {};
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                if (raw.header.dwType == RIM_TYPEMOUSE) {
                    const RAWMOUSE& m = raw.data.mouse;
                    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                        input::AddMouseDelta(m.lLastX, m.lLastY);
                    }
                    if (m.usButtonFlags & RI_MOUSE_WHEEL) {
                        input::AddWheel((float)(short)m.usButtonData /
                                        (float)WHEEL_DELTA);
                    }
                }
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1;  // 内容全由 DComp 合成，别让 GDI 刷一层底色

        case WM_MOUSEACTIVATE:
            // 永远不激活。点面板只是"点一层贴纸"，游戏不能因此失焦。
            return MA_NOACTIVATE;

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
            // 面板打开期间每秒重申一次鼠标归属：有些引擎发现鼠标设备变化
            // 会自己重新注册 Raw Input，把我们的冻结顶掉
            if (gui::Visible()) {
                const ULONGLONG now = GetTickCount64();
                if (now >= g_raw_reassert_at) {
                    g_raw_reassert_at = now + 1000;
                    HookMouseToOverlay(true);
                }
            }
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

            // 抢焦点后确认：客户端有没有因为失焦把窗口改成全屏 / 缩小
            if (g_rect_check_at != 0 && GetTickCount64() >= g_rect_check_at) {
                g_rect_check_at = 0;
                RECT now = {};
                if (g_game != nullptr && IsWindow(g_game) && GetWindowRect(g_game, &now)) {
                    const int ow = g_game_rect_before.right - g_game_rect_before.left;
                    const int oh = g_game_rect_before.bottom - g_game_rect_before.top;
                    const int nw = now.right - now.left;
                    const int nh = now.bottom - now.top;
                    if (ow != nw || oh != nh) {
                        LogLine("[overlay] 注意：抢焦点后游戏窗口尺寸 %dx%d -> %dx%d，"
                                "客户端自己改了显示模式", ow, oh, nw, nh);
                    }
                }
            }

            // 关面板交还焦点后确认一次。
            // 判断依据是"系统光标有没有被游戏藏起来"：这个客户端不用 ClipCursor，
            // 它锁视角的方式是 ShowCursor(FALSE) + Raw Input 相对位移，
            // 所以 cursor_clipped 在这里恒为 0，拿它判断永远看不出问题。
            if (g_release_check_at != 0 && GetTickCount64() >= g_release_check_at) {
                g_release_check_at = 0;
                const bool vis = input::CursorVisible();
                LogLine("[overlay] 交还检查：cursor_visible=%d（0 = 游戏已重新隐藏光标，捕获正常）",
                        vis ? 1 : 0);
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

    // 退出前必须把鼠标 Raw Input 还给游戏，否则游戏视角会一直锁着
    HookMouseToOverlay(false);
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
