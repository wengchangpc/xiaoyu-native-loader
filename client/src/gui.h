// gui.h - ClickGui 覆盖层
#pragma once

namespace xiaoyu {
namespace gui {

void Init();
void Shutdown();

void Toggle();
bool Visible();
void SetVisible(bool v);

// 每帧（Present 里）调用
void OnFrame();

}  // namespace gui
}  // namespace xiaoyu
