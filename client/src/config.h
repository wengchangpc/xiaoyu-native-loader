// config.h - 配置持久化
#pragma once

#include <string>
#include <vector>

namespace xiaoyu {
namespace cfg {

void Load();
void Save();
void MarkDirty();
void Tick();  // 有改动时延迟落盘，避免拖滑块时每帧写文件

// 面板整体缩放（原版 self._s() 的对应物）
float Scale();
void SetScale(float s);

bool Chinese();
void SetChinese(bool v);

// 模块开关：按 kFeatures 下标存
bool ModuleOn(int index);
void SetModuleOn(int index, bool on);

int ModuleBind(int index);
void SetModuleBind(int index, int vk);

// 参数值：按 kParams 下标存
float ParamValue(int index, float def);
void SetParamValue(int index, float v);

// 这个参数有没有被显式写过。
// 用途：滑块的"默认值"是区间中点（见 gui.cpp ParamDefault），直接拿来当默认
// 会让颜色变成灰、位置跑到屏幕正中。HUD 那边要靠它区分"用户调过"和"没动过"。
bool ParamIsSet(int index);

// 把显式值删掉，回到出厂默认（HUD 拖动界面的右键复位用它）。
void ClearParam(int index);

// 面板位置（用户拖过才有效）
bool PanelPos(int group, float* x, float* y, bool* collapsed);
void SetPanelPos(int group, float x, float y, bool collapsed);

bool HudEnabled(int feature_index);
void SetHudEnabled(int feature_index, bool on);

// 鼠标穿透：GUI 打开时是否独占输入
bool BlockInput();
void SetBlockInput(bool v);

// 打开面板时是否把前台从游戏手里接过来（默认开）。
// 游戏失焦后会自己放开鼠标捕获，关掉面板再把前台还回去，视角立刻恢复。
// 这是主路径，不依赖任何键位绑定。
bool TakeFocus();
void SetTakeFocus(bool v);

// 兼容旧行为：合成游戏的"释放鼠标"键。默认关 —— 实测连按两次容易把游戏
// 卡在光标模式里出不来（关了面板也不能转视角）。只有 focus=0 且确实需要时才开。
// 如果哪天游戏改绑定、或合成的 Alt 反而把鼠标锁回去，把它设成 0 就能退回手动。
bool AutoUnlock();
void SetAutoUnlock(bool v);

// 合成哪个键来解锁鼠标，默认 VK_LMENU（左 Alt）
int UnlockKey();
void SetUnlockKey(int vk);

// ── 配置档 ──────────────────────────────────────────────────────
// 测试时经常要在"打架一套 / 挂机一套"之间来回切，单一配置不够用。
// 1..3 三个槽，切档时先自动保存当前档再读目标档，所以不会丢改动。
int CurrentProfile();            // 0 = 默认档
void SwitchProfile(int slot);    // slot: 1..3，等于当前档时无动作

}  // namespace cfg
}  // namespace xiaoyu
