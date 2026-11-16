// input.h - 窗口消息子类化 + 输入状态
#pragma once

#include <windows.h>

namespace xiaoyu {
namespace input {

bool Attach(HWND hwnd);
void Detach();

// GUI 打开时把输入从游戏手里抢过来
void SetCapture(bool on);
bool IsCapturing();

// 每帧开头调用：刷新鼠标位置、清掉 pressed/released/wheel 的边沿状态
void NewFrame();

float X();
float Y();
float Wheel();

bool Down(int btn);      // 0 左 1 右 2 中
bool Pressed(int btn);
bool Released(int btn);

bool KeyDown(int vk);
bool KeyPressed(int vk);

// 只取一次：取完清掉，用于"点一下绑定快捷键"
int TakePressedKey();

void SetHotkey(int vk);
int Hotkey();
bool HotkeyPressed();

}  // namespace input
}  // namespace xiaoyu
