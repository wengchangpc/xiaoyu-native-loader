// input.h - 输入状态（轮询驱动）
//
// 演进说明：
//   原来是通过 SetWindowLongPtr 子类化游戏窗口来收消息的。改成独立覆盖窗口
//   之后，这条路没了意义 —— 覆盖窗口平时是"点击穿透"的（WM_NCHITTEST 返回
//   HTTRANSPARENT），根本收不到鼠标消息。
//
//   所以改成主动轮询 GetAsyncKeyState：不依赖焦点、不依赖窗口消息，
//   游戏抢了焦点也照样能读到按键。滚轮是唯一必须靠消息的东西，
//   由覆盖窗口的 WndProc 调 AddWheel() 喂进来。
#pragma once

#include <windows.h>

namespace xiaoyu {
namespace input {

// 记录覆盖窗口句柄，鼠标坐标以它为客户区原点
bool Attach(HWND overlay_hwnd);
void Detach();

// GUI 打开时把输入从游戏手里抢过来。
// 新架构下真正的输入接管由覆盖窗口的命中测试负责（见 overlay.cpp），
// 这里只保留状态位，供 gui.cpp 沿用原有调用。
void SetCapture(bool on);
bool IsCapturing();

// 每帧开头调用：刷新鼠标位置，算 pressed/released 边沿，清空滚轮
void NewFrame();

float X();
float Y();
float Wheel();
void AddWheel(float delta);

bool Down(int btn);      // 0 左 1 右 2 中 3 X1 4 X2
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
