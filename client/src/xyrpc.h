// rpc.h - 进程内命令通道（命名管道）+ 脚本绘制列表
//
// 定位：
//   bridge.cpp 那套"往 run.bat 放文件"适合跑一次性的外部程序（侦察脚本），
//   但每次都要落盘、等 500ms 轮询、再读日志，做交互式调试太笨。
//   这里再开一条常驻管道 \\.\pipe\xiaoyu_rpc，用一行一条命令的方式直接
//   读写游戏内存、枚举实体、改参数、画图。
//
//   ★ 关键点：内存读写在**游戏进程内**完成（走 mem::Read，带 SEH 兜底）。
//     外部进程直接 ReadProcessMemory 扫堆会被反作弊几秒内杀掉，而脚本
//     通过这条管道请求、由 DLL 代读，行为等同于 DLL 自己读，安全得多。
//
// 协议（全 ASCII，一行一条）：
//     请求  <cmd> [args...]\n
//     应答  OK <len>\n<len 字节负载>     或     ERR <原因>\n
//
// 命令表见 rpc.cpp 里的 Dispatch()。
#pragma once

namespace xiaoyu {
namespace rpc {

void Start();
void Stop();

// 覆盖层每帧调用，把脚本提交的绘制列表画出来（在 GUI 之前）
void RenderDrawList();

// 有没有挂着的客户端（调参时观察用）
int ClientCount();

}  // namespace rpc
}  // namespace xiaoyu
