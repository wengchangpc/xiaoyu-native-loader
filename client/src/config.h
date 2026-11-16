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

// 面板位置（用户拖过才有效）
bool PanelPos(int group, float* x, float* y, bool* collapsed);
void SetPanelPos(int group, float x, float y, bool collapsed);

bool HudEnabled(int feature_index);
void SetHudEnabled(int feature_index, bool on);

// 鼠标穿透：GUI 打开时是否独占输入
bool BlockInput();
void SetBlockInput(bool v);

}  // namespace cfg
}  // namespace xiaoyu
