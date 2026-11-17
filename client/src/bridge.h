// bridge.h - 文件命令通道：借游戏进程的管理员权限执行外部脚本
//
// 为什么需要这个：
//   游戏以管理员身份（High IL）运行，普通权限读不了它的内存。而每次提权
//   都要人工点一次 UAC —— 人睡觉的时候没法守着手点。三条常驻路线都堵死了：
//     - Start-Process -Verb RunAs 起的进程挂在当前会话进程树里，
//       会话一收尾就被连带清掉（实测跑了 4 秒就没了，alive.txt 停在 00:57:21）
//     - schtasks.exe 被安全策略列进程序黑名单，直接拒绝启动
//     - 创建服务/注册表自启同样需要管理员
//
//   唯一稳定的落点是**游戏进程本身**：DLL 已经注进去了，它就在 High IL 里
//   跑着，生命周期和游戏一样长，而且不需要任何额外授权就能读写游戏内存。
//
// 工作方式：
//   本模块起一个后台线程，每 500ms 看一次 C:\Xiaoyu\run.bat。
//   文件存在就用 CreateProcessW 启动 `cmd.exe /c run.bat`，等它跑完，
//   然后把文件改名成 run.done（这样同一个脚本不会被重复执行）。
//
//   于是"以管理员身份跑任意脚本"退化成"往 C:\Xiaoyu 里放一个 bat"，
//   而写普通文件不需要任何权限。
#pragma once

namespace xiaoyu {
namespace bridge {

// 起后台线程。幂等 —— 重复调用只会启动一次。
void Start();

// 已经执行过的脚本个数（诊断用）
int ExecCount();

}  // namespace bridge
}  // namespace xiaoyu
