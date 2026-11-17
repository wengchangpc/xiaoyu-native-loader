// gui.h - ClickGui 覆盖层
#pragma once

namespace xiaoyu {
namespace gui {

void Init();
void Shutdown();

void Toggle();
bool Visible();
void SetVisible(bool v);

// 面板或 HUD 编辑模式任意一个开着，都需要接管鼠标/键盘。
// overlay 那边所有"要不要吃鼠标"的判断都必须走这里，不能只看 Visible()，
// 否则 HUD 编辑模式下游戏仍在转视角、光标仍是隐藏的，根本拖不动。
bool WantsMouse();

// HUD 屏内编辑：直接拖屏幕上的 FPS / CPS / ArrayList / Keystrokes 摆位置，
// 比回头去找 posx 滑块直观得多（对应原版的 HudEditScreen）。
bool HudEdit();
void SetHudEdit(bool v);

// 每帧（Present 里）调用
void OnFrame();

}  // namespace gui
}  // namespace xiaoyu
