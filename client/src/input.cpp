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

int g_take_key = 0;

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
    g_mx += (float)dx;
    g_my += (float)dy;
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
    for (int vk = 8; vk < kMaxKey; ++vk) {
        bool now = Raw(vk);
        g_key_pressed[vk] = now && !g_prev_key[vk];
        g_key_down[vk] = now;
        if (g_key_pressed[vk]) {
            if (vk == g_hotkey) {
                g_hotkey_pressed = true;
            } else if (vk != VK_ESCAPE) {
                // 留给"点一下绑定快捷键"
                g_take_key = vk;
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
    int k = g_take_key;
    g_take_key = 0;
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

// ── 低级鼠标钩子 ────────────────────────────────────────────────
// 这里曾经用 RegisterRawInputDevices(RIDEV_INPUTSINK) 收滚轮，那是个致命错误：
// Raw Input 的注册是按 (usagePage, usage) 的，同一个进程里只能有一份。
// 我们注册鼠标、关面板时又 RIDEV_REMOVE，等于把**游戏自己的鼠标注册**一起删了 ——
// 游戏靠它读相对位移转视角，删掉之后视角就永远不能转了。
//
// WH_MOUSE_LL 是另一条通道，完全不碰 Raw Input 的注册表，安全。
HHOOK g_hook = nullptr;
bool g_want_wheel = false;
bool g_want_phys = false;
volatile LONG g_phys_left = 0;

LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (ms != nullptr) {
            if (wp == WM_MOUSEWHEEL) {
                if (g_want_wheel) {
                    const short delta = (short)HIWORD(ms->mouseData);
                    AddWheel((float)delta / (float)WHEEL_DELTA);
                }
            } else if ((ms->flags & LLMHF_INJECTED) == 0) {
                if (wp == WM_LBUTTONDOWN) g_phys_left = 1;
                else if (wp == WM_LBUTTONUP) g_phys_left = 0;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void UpdateHook() {
    const bool need = g_want_wheel || g_want_phys;
    if (need && g_hook == nullptr) {
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
        if (g_hook == nullptr) {
            LogLine("[input] 装鼠标钩子失败 err=%lu", (unsigned long)GetLastError());
        } else {
            LogLine("[input] 鼠标钩子已挂上 wheel=%d phys=%d", g_want_wheel ? 1 : 0,
                    g_want_phys ? 1 : 0);
        }
    } else if (!need && g_hook != nullptr) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_phys_left = 0;
    }
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
    if (g_want_wheel == on) return;
    g_want_wheel = on;
    UpdateHook();
}

void SetPhysicalWanted(bool on) {
    if (g_want_phys == on) return;
    g_want_phys = on;
    UpdateHook();
}

bool PhysLeftDown() { return g_phys_left != 0; }

}  // namespace input
}  // namespace xiaoyu
