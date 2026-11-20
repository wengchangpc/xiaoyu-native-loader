// modules.h - 模块运行时
//
// 只做「不依赖游戏内存」的那一类模块：它们的效果全部通过向系统合成输入
// 实现（SendInput），因此不需要逆向 Themida 加壳后的偏移表，也不会因为
// 客户端更新而失效。
//
// 需要读游戏内存的模块（KillAura / Fly / ESP / Scaffold / Xray ...）不在这里，
// 那些得先做特征码扫描，属于另一个阶段。
#pragma once

namespace xiaoyu {
namespace modules {

// 每帧调用一次。内部按 catalog 里的模块名找开关和参数，
// 面板打开时自动停手并释放所有合成按键（否则抢焦点会被自己的点击打断）。
void Tick();

// 立刻松开所有由本模块按下的键/鼠标（面板打开、关闭、退出时调用）
void ReleaseAll();

// 把被 Zoom / BigFov 改写过的投影矩阵写回游戏原值。
// 卸载前必须调：投影是**缓存值**，我们改完之后游戏不会自己重算，
// DLL 一走就没人维持了，画面会一直停在放大/广角状态。
void RestoreProjection();

// 这个模块名对应的功能是否真的实现了（GUI 自己那一套也算）。
// 用来在切换模块时提示"还没实现"，免得用户对着空壳开关发懵。
bool Implemented(const wchar_t* english_name);

}  // namespace modules
}  // namespace xiaoyu
