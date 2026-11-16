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

// 往当前前台窗口合成一次按键（按下+松开）。
// 用来替用户按游戏的"释放鼠标"键，免得每次开面板都要手动按一下。
void SendKeyTap(int vk);

// 鼠标是否还被裁剪在某个矩形里。
// 注意：这个客户端根本不用 ClipCursor，它靠"隐藏系统光标 + Raw Input 相对位移"
// 来锁视角，所以这个函数在它身上恒为 false，不能拿来判断"是否已捕获"。
bool IsCursorClipped();

// 系统光标现在是否可见。游戏处于鼠标捕获模式时会 ShowCursor(FALSE)，
// 所以这个才是判断"游戏有没有抓住鼠标"的可靠指标。
bool CursorVisible();

// 低级鼠标钩子（WH_MOUSE_LL）的两个用途，按需开关，内部引用计数。
//   SetWheelWanted  : 面板打开期间收滚轮（钩子不影响 Raw Input，
//                      这一点非常关键 —— 见 overlay.cpp 里旧代码的注释）
//   SetPhysicalWanted: 自动连点需要知道"物理左键是不是按着"，
//                      要靠 MSLLHOOKSTRUCT.flags 里的 LLMHF_INJECTED 过滤自己注入的事件
void SetWheelWanted(bool on);
void SetPhysicalWanted(bool on);

// 物理（非注入）左键是否按下
bool PhysLeftDown();

}  // namespace input
}  // namespace xiaoyu
