// self.h - 模块自身状态与卸载
//
// 为什么需要"卸载"：
//   同路径重复注入不会再次执行 DllMain（Windows 认为模块已在进程里），所以
//   每迭代一版就得换一个文件名。看着一次几百 KB 不大，但每个实例都会
//   建一个覆盖窗口、装一套鼠标键盘钩子、起三个线程，叠到四五个之后：
//     - 屏幕上会同时存在好几层透明窗口
//     - 每层都在轮询 run.bat（命令可能被执行两次）
//     - 钩子互相抢，按键行为变得难以预测
//   所以必须有办法让旧实例彻底停下来。
//
// 两种卸载：
//   软卸载  停掉 RPC / 命令通道 / 覆盖层 / 钩子，DLL 仍驻留但不做任何事。
//           任何时候都安全，推荐默认用这个。
//   硬卸载  在上面基础上 FreeLibraryAndExitThread。要求命令通道此刻没在跑
//           子进程（跑着的话子进程返回时会跳回已释放的代码段），否则自动
//           退化成软卸载。
#pragma once

#include <windows.h>

namespace xiaoyu {
namespace self {

// DllMain 里把 hModule 存进来
void SetModule(HMODULE m);
HMODULE Module();

// 异步请求卸载。hard=false 只停活动，hard=true 尝试真正 FreeLibrary。
// 返回值：false 表示已经请求过了。
bool RequestUnload(bool hard);

bool UnloadRequested();

}  // namespace self
}  // namespace xiaoyu
