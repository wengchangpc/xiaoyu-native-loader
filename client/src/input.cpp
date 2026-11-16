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

void NewFrame() {
    UpdateCursorPos();

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

bool IsCursorClipped() {
    RECT clip;
    if (!GetClipCursor(&clip)) return false;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int cw = (int)(clip.right - clip.left);
    int ch = (int)(clip.bottom - clip.top);
    return cw < sw - 2 || ch < sh - 2;
}

}  // namespace input
}  // namespace xiaoyu
