// input.cpp
#include "input.h"

#include <windowsx.h>
#include <string.h>

#include "log.h"

namespace xiaoyu {
namespace input {
namespace {

const int kMaxBtn = 5;
const int kMaxKey = 256;

HWND g_hwnd = nullptr;
WNDPROC g_orig_proc = nullptr;
bool g_capture = false;

float g_mx = 0.0f;
float g_my = 0.0f;
float g_wheel = 0.0f;

bool g_down[kMaxBtn] = {false};
bool g_pressed[kMaxBtn] = {false};
bool g_released[kMaxBtn] = {false};

bool g_key_down[kMaxKey] = {false};
bool g_key_pressed[kMaxKey] = {false};

int g_hotkey = VK_RSHIFT;
bool g_hotkey_pressed = false;
int g_take_key = 0;

int BtnIndex(UINT msg, bool up) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
            return 1;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
            return 2;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return 3;
        default:
            (void)up;
            return -1;
    }
}

void UpdateCursorPos() {
    POINT p;
    if (GetCursorPos(&p) && g_hwnd != nullptr) {
        ScreenToClient(g_hwnd, &p);
        g_mx = (float)p.x;
        g_my = (float)p.y;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE:
            g_mx = (float)GET_X_LPARAM(lp);
            g_my = (float)GET_Y_LPARAM(lp);
            if (g_capture) return 0;
            break;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            int b = BtnIndex(msg, false);
            if (b >= 0 && b < kMaxBtn) {
                g_down[b] = true;
                g_pressed[b] = true;
            }
            if (g_capture) return 0;
            break;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            int b = BtnIndex(msg, true);
            if (b >= 0 && b < kMaxBtn) {
                g_down[b] = false;
                g_released[b] = true;
            }
            if (g_capture) return 0;
            break;
        }

        case WM_MOUSEWHEEL:
            g_wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            if (g_capture) return 0;
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)wp;
            if (vk >= 0 && vk < kMaxKey) {
                if (!g_key_down[vk]) g_key_pressed[vk] = true;
                g_key_down[vk] = true;
            }
            if (vk == g_hotkey) g_hotkey_pressed = true;
            if (vk != g_hotkey && vk != VK_ESCAPE) g_take_key = vk;
            if (g_capture) {
                // 绑定快捷键时也要能把 ESC 吞掉
                return 0;
            }
            break;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            int vk = (int)wp;
            if (vk >= 0 && vk < kMaxKey) g_key_down[vk] = false;
            if (g_capture) return 0;
            break;
        }

        case WM_KILLFOCUS:
            memset(g_down, 0, sizeof(g_down));
            memset(g_key_down, 0, sizeof(g_key_down));
            break;

        default:
            break;
    }

    if (g_orig_proc != nullptr) {
        return CallWindowProcW(g_orig_proc, h, msg, wp, lp);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool Attach(HWND hwnd) {
    if (hwnd == nullptr) return false;
    if (g_hwnd == hwnd) return true;
    if (g_hwnd != nullptr) Detach();

    g_hwnd = hwnd;
    g_orig_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
    if (g_orig_proc == nullptr) {
        LogLine("[input] SetWindowLongPtr failed err=%lu", (unsigned long)GetLastError());
        g_hwnd = nullptr;
        return false;
    }
    LogLine("[input] attached to hwnd=%p", (void*)hwnd);
    return true;
}

void Detach() {
    if (g_hwnd == nullptr || g_orig_proc == nullptr) return;
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_proc);
    g_hwnd = nullptr;
    g_orig_proc = nullptr;
}

void SetCapture(bool on) { g_capture = on; }
bool IsCapturing() { return g_capture; }

void NewFrame() {
    g_wheel = 0.0f;
    g_hotkey_pressed = false;
    memset(g_pressed, 0, sizeof(g_pressed));
    memset(g_released, 0, sizeof(g_released));
    memset(g_key_pressed, 0, sizeof(g_key_pressed));
    UpdateCursorPos();
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

}  // namespace input
}  // namespace xiaoyu
