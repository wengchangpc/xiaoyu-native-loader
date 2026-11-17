// input.cpp - 输入状态（GetAsyncKeyState 轮询）
#include "input.h"

#include <string.h>

#include "log.h"

namespace xiaoyu {
namespace input {
namespace {

const int kMaxBtn = 5;
const int kMaxKey = 256;

HWND g_hwnd = nullptr;

float g_mx = 0.0f;
float g_my = 0.0f;
float g_wheel = 0.0f;

bool g_down[kMaxBtn] = {false};
bool g_pressed[kMaxBtn] = {false};
bool g_released[kMaxBtn] = {false};
bool g_prev_btn[kMaxBtn] = {false};

bool g_key_down[kMaxKey] = {false};
bool g_key_pressed[kMaxKey] = {false};
bool g_prev_key[kMaxKey] = {false};

int g_hotkey = VK_RSHIFT;
bool g_hotkey_pressed = false;

// 本帧新按下的键排队，留给"点一下绑定快捷键"（gui 的 PK_KEYBIND 行）来取。
//
// 以前这里只有一个 int g_take_key，谁最后被扫到就留谁，而且 ESC 被显式排除。
// 两个后果：
//   1) 一帧里同时按下两个键，绑定结果取决于扫描顺序（VK 大的赢），不可预期；
//   2) ESC 永远进不来，于是 gui 里那条 "if (vk == VK_ESCAPE) 取消绑定"
//      的分支是死代码 —— 取消绑定实际上从来没生效过。
// 现在改成小队列，并且不再过滤 ESC（要不要当成取消，交给消费端判断）。
const int kKeyQueue = 4;
int g_key_queue[kKeyQueue] = {0};
int g_key_queue_count = 0;

// 新架构下输入接管靠窗口命中测试，这里只留状态位
bool g_capture = false;

// 物理按键 -> VK。用普通 VK_LBUTTON 等，GetAsyncKeyState 直接支持。
const int kBtnVk[kMaxBtn] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};

bool Raw(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void UpdateCursorPos() {
    POINT p;
    if (!GetCursorPos(&p)) return;
    if (g_hwnd != nullptr) {
        // 用屏幕坐标减去窗口客户区原点，不依赖 WM_MOUSEMOVE
        POINT origin = {0, 0};
        if (ClientToScreen(g_hwnd, &origin)) {
            g_mx = (float)(p.x - origin.x);
            g_my = (float)(p.y - origin.y);
        }
    } else {
        g_mx = (float)p.x;
        g_my = (float)p.y;
    }
}

}  // namespace

bool Attach(HWND overlay_hwnd) {
    if (overlay_hwnd == nullptr) return false;
    g_hwnd = overlay_hwnd;
    memset(g_down, 0, sizeof(g_down));
    memset(g_prev_btn, 0, sizeof(g_prev_btn));
    memset(g_key_down, 0, sizeof(g_key_down));
    memset(g_prev_key, 0, sizeof(g_prev_key));
    LogLine("[input] polling mode, overlay hwnd=%p", (void*)overlay_hwnd);
    return true;
}

void Detach() {
    g_hwnd = nullptr;
}

void SetCapture(bool on) { g_capture = on; }
bool IsCapturing() { return g_capture; }

void AddWheel(float delta) {
    g_wheel += delta;
}

bool g_relative = false;

void SetRelativeMode(bool on) {
    if (g_relative == on) return;
    if (on) UpdateCursorPos();   // 以当前真实光标位置为起点，避免第一帧跳一下
    g_relative = on;
    LogLine("[input] 相对光标模式 %d，起点 (%.0f,%.0f)", on ? 1 : 0, g_mx, g_my);
}

bool RelativeMode() { return g_relative; }

void AddMouseDelta(int dx, int dy) {
    if (!g_relative) return;
    // Raw Input 的位移是"鼠标计数"，和像素不是 1:1：系统鼠标加速、DPI、
    // 游戏里的灵敏度都不影响它，但玩家的手感是按光标走的。乘一个系数
    // 让面板上的光标跟手一点，1.5 是试出来的折中值。
    const float kSens = 1.5f;
    g_mx += (float)dx * kSens;
    g_my += (float)dy * kSens;
    // 钳在客户区内：跑出去之后面板就点不到了
    if (g_hwnd != nullptr) {
        RECT rc = {};
        if (GetClientRect(g_hwnd, &rc)) {
            const float w = (float)(rc.right - 1);
            const float h = (float)(rc.bottom - 1);
            if (g_mx < 0.0f) g_mx = 0.0f;
            if (g_my < 0.0f) g_my = 0.0f;
            if (g_mx > w) g_mx = w;
            if (g_my > h) g_my = h;
        }
    }
}

void NewFrame() {
    // 相对模式下不能用 GetCursorPos：游标被按住不动，读出来永远是同一个点
    if (!g_relative) UpdateCursorPos();

    // 鼠标按键：边沿检测
    for (int b = 0; b < kMaxBtn; ++b) {
        bool now = Raw(kBtnVk[b]);
        g_pressed[b] = now && !g_prev_btn[b];
        g_released[b] = (!now) && g_prev_btn[b];
        g_down[b] = now;
        g_prev_btn[b] = now;
    }

    // 键盘：只扫 8..255（0-7 是鼠标键，1/2/4 是左右中键的别名，跳过）
    g_hotkey_pressed = false;
    g_key_queue_count = 0;
    for (int vk = 8; vk < kMaxKey; ++vk) {
        bool now = Raw(vk);
        g_key_pressed[vk] = now && !g_prev_key[vk];
        g_key_down[vk] = now;
        if (g_key_pressed[vk]) {
            if (vk == g_hotkey) {
                g_hotkey_pressed = true;
            } else if (g_key_queue_count < kKeyQueue) {
                g_key_queue[g_key_queue_count++] = vk;
            }
        }
        g_prev_key[vk] = now;
    }
}

float X() { return g_mx; }
float Y() { return g_my; }
float Wheel() { return g_wheel; }

bool Down(int btn) {
    if (btn < 0 || btn >= kMaxBtn) return false;
    return g_down[btn];
}
bool Pressed(int btn) {
    if (btn < 0 || btn >= kMaxBtn) return false;
    return g_pressed[btn];
}
bool Released(int btn) {
    if (btn < 0 || btn >= kMaxBtn) return false;
    return g_released[btn];
}

bool KeyDown(int vk) {
    if (vk < 0 || vk >= kMaxKey) return false;
    return g_key_down[vk];
}
bool KeyPressed(int vk) {
    if (vk < 0 || vk >= kMaxKey) return false;
    return g_key_pressed[vk];
}

int TakePressedKey() {
    if (g_key_queue_count <= 0) return 0;
    const int k = g_key_queue[0];
    for (int i = 1; i < g_key_queue_count; ++i) g_key_queue[i - 1] = g_key_queue[i];
    --g_key_queue_count;
    g_key_queue[g_key_queue_count] = 0;
    return k;
}

void SetHotkey(int vk) { g_hotkey = vk; }
int Hotkey() { return g_hotkey; }
bool HotkeyPressed() { return g_hotkey_pressed; }

void SendKeyTap(int vk) {
    if (vk == 0) return;
    INPUT in[2] = {};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = (WORD)vk;
        in[i].ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        in[i].ki.dwFlags = (i == 1) ? KEYEVENTF_KEYUP : 0;
    }
    UINT sent = SendInput(2, in, sizeof(INPUT));
    if (sent != 2) {
        LogLine("[input] SendInput(vk=0x%02X) 只送出了 %u/2, err=%lu", vk,
                (unsigned)sent, (unsigned long)GetLastError());
    }
}

// ── 低级钩子（鼠标 + 键盘）──────────────────────────────────────
// 踩过的两个坑，都别再来一次：
//
// 坑一：用 RegisterRawInputDevices(RIDEV_INPUTSINK) 收滚轮。
//   Raw Input 的注册按 (usagePage, usage) 在进程内唯一，注册我们这份等于
//   顶掉游戏那份；关面板时再 RIDEV_REMOVE 就把**游戏自己的注册**一起删了，
//   游戏再也收不到相对位移，视角永久锁死。
//
// 坑二（本次）：把 WH_MOUSE_LL 装在覆盖层渲染线程上。
//   低级钩子的回调是**同步**的 —— 系统把事件派给安装钩子的那个线程，
//   回调返回之前全系统的鼠标输入都堵在那儿。渲染线程每帧画完还要
//   Sleep(最多 50ms)，于是整台机器的鼠标一卡一卡，游戏挂后台、在浏览器里
//   操作也照样卡，只有关掉游戏（DLL 卸载、钩子被系统清掉）才恢复。
//
// 所以：钩子一律由下面这个专用线程持有。它除了阻塞等消息什么都不干，
// 回调里只读一个字段就返回，不给系统输入增加任何可感知延迟。

HHOOK g_hook = nullptr;
volatile LONG g_want_wheel = 0;
volatile LONG g_want_phys = 0;
volatile LONG g_phys_left = 0;
volatile LONG g_phys_right = 0;

HHOOK g_kb_hook = nullptr;
volatile LONG g_want_phys_key = 0;
volatile LONG g_phys_space = 0;

HANDLE g_hook_thread = nullptr;
HANDLE g_hook_ready = nullptr;
DWORD g_hook_tid = 0;
const UINT WM_HOOK_SYNC = WM_APP + 0x51;
const UINT WM_HOOK_QUIT = WM_APP + 0x52;

// 取"我们自己的模块句柄"。低级钩子的 hMod 应该指向钩子过程所在的模块；
// 传 GetModuleHandleW(nullptr) 拿到的是游戏 exe，虽然 LL 钩子不跨进程注入、
// 多数情况下也能work，但用错模块句柄属于约定不符，稳一点按地址反查。
HMODULE SelfModule(const void* addr) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &m);
    return m;
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    // 这个函数跑在钩子线程上，必须极快返回。只做字段读写，不打日志、不加锁。
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (ms != nullptr) {
            if (wp == WM_MOUSEWHEEL) {
                if (g_want_wheel != 0) {
                    const short delta = (short)HIWORD(ms->mouseData);
                    AddWheel((float)delta / (float)WHEEL_DELTA);
                }
            } else if ((ms->flags & LLMHF_INJECTED) == 0) {
                if (wp == WM_LBUTTONDOWN) g_phys_left = 1;
                else if (wp == WM_LBUTTONUP) g_phys_left = 0;
                else if (wp == WM_RBUTTONDOWN) g_phys_right = 1;
                else if (wp == WM_RBUTTONUP) g_phys_right = 0;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK KeyHookProc(int code, WPARAM wp, LPARAM lp) {
    // 为什么需要它：NoJumpDelay 要「按住空格时反复顶一次跳」，就得知道
    // **物理上**空格是不是按着。GetAsyncKeyState 做不到 —— 我们为了顶跳会
    // 成对注入 UP/DOWN，注入的 DOWN 会把状态表写成"按下"，玩家真松手之后
    // 仍然读到按下，于是空格被永久按住、人一直跳。
    // 和 AC 判物理左键是同一个坑，解法也一样：LL 钩子 + LLKHF_INJECTED 过滤。
    // 只关心空格，其他键原样放行。
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (kb != nullptr && (kb->flags & LLKHF_INJECTED) == 0) {
            if (kb->vkCode == VK_SPACE) {
                if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                    g_phys_space = 1;
                } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                    g_phys_space = 0;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// 只在钩子线程上调。
void ApplyHookState() {
    // 鼠标钩子
    const bool need_mouse = (g_want_wheel != 0) || (g_want_phys != 0);
    if (need_mouse && g_hook == nullptr) {
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, SelfModule((const void*)&MouseHookProc), 0);
        if (g_hook == nullptr) {
            LogLine("[input] 装鼠标钩子失败 err=%lu", (unsigned long)GetLastError());
        } else {
            LogLine("[input] 鼠标钩子已挂上（专用线程）");
        }
    } else if (!need_mouse && g_hook != nullptr) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_phys_left = 0;
        g_phys_right = 0;
    }

    // 键盘钩子
    if (g_want_phys_key != 0 && g_kb_hook == nullptr) {
        g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, SelfModule((const void*)&KeyHookProc), 0);
        if (g_kb_hook == nullptr) {
            LogLine("[input] 装键盘钩子失败 err=%lu", (unsigned long)GetLastError());
        } else {
            LogLine("[input] 键盘钩子已挂上（专用线程，只跟踪空格）");
        }
    } else if (g_want_phys_key == 0 && g_kb_hook != nullptr) {
        UnhookWindowsHookEx(g_kb_hook);
        g_kb_hook = nullptr;
        g_phys_space = 0;
    }
}

DWORD WINAPI HookThreadProc(LPVOID param) {
    (void)param;
    g_hook_tid = GetCurrentThreadId();
    MSG msg;
    // 先 Peek 一次把消息队列建出来，否则后面的 PostThreadMessage 会失败
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    if (g_hook_ready != nullptr) SetEvent(g_hook_ready);
    ApplyHookState();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOOK_QUIT) break;
        if (msg.message == WM_HOOK_SYNC) {
            ApplyHookState();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 退出前无条件摘掉，别把钩子留在系统里
    if (g_hook != nullptr) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    if (g_kb_hook != nullptr) { UnhookWindowsHookEx(g_kb_hook); g_kb_hook = nullptr; }
    g_phys_left = 0;
    g_phys_right = 0;
    g_phys_space = 0;
    g_hook_tid = 0;
    return 0;
}

// 需求变化时调：第一次会把线程拉起来，之后只是让线程重新评估装不装钩子。
void SyncHookThread() {
    if (g_hook_thread == nullptr) {
        g_hook_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hook_thread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
        if (g_hook_thread == nullptr) {
            LogLine("[input] 钩子线程创建失败 err=%lu", (unsigned long)GetLastError());
            if (g_hook_ready != nullptr) { CloseHandle(g_hook_ready); g_hook_ready = nullptr; }
            return;
        }
        if (g_hook_ready != nullptr) {
            WaitForSingleObject(g_hook_ready, 2000);
            CloseHandle(g_hook_ready);
            g_hook_ready = nullptr;
        }
        return;   // 线程启动时已经 Apply 过一次
    }
    if (g_hook_tid != 0) PostThreadMessageW(g_hook_tid, WM_HOOK_SYNC, 0, 0);
}

bool IsCursorClipped() {
    RECT clip;
    if (!GetClipCursor(&clip)) return false;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int cw = (int)(clip.right - clip.left);
    int ch = (int)(clip.bottom - clip.top);
    return cw < sw - 2 || ch < sh - 2;
}

bool CursorVisible() {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return true;   // 查不到就当可见，宁可多报
    return (ci.flags & CURSOR_SHOWING) != 0;
}

void SetWheelWanted(bool on) {
    if ((g_want_wheel != 0) == on) return;
    g_want_wheel = on ? 1 : 0;
    SyncHookThread();
}

void SetPhysicalWanted(bool on) {
    if ((g_want_phys != 0) == on) return;
    g_want_phys = on ? 1 : 0;
    SyncHookThread();
}

void SetPhysicalKeyWanted(bool on) {
    if ((g_want_phys_key != 0) == on) return;
    g_want_phys_key = on ? 1 : 0;
    SyncHookThread();
}

bool PhysLeftDown() { return g_phys_left != 0; }

bool PhysRightDown() { return g_phys_right != 0; }

bool PhysSpaceDown() { return g_phys_space != 0; }

void ShutdownHooks() {
    if (g_hook_thread == nullptr) return;
    if (g_hook_tid != 0) PostThreadMessageW(g_hook_tid, WM_HOOK_QUIT, 0, 0);
    WaitForSingleObject(g_hook_thread, 3000);
    CloseHandle(g_hook_thread);
    g_hook_thread = nullptr;
    g_hook_tid = 0;
}

}  // namespace input
}  // namespace xiaoyu
