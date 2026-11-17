// probe.h - 进程内自诊断
//
// 为什么要有这个东西：
//   从外部（另一个进程）用 ReadProcessMemory 扫这个客户端是可以读到主模块的
//   ——实测 dump 出 217MB、`.text` 熵从磁盘上的 7.97 降到 6.88，说明壳的
//   解密确实发生在运行时。但只要改成遍历堆去找对象，扫描进程就会在几秒内
//   被静默终止，日志停在扫描途中、连异常都没来得及写。
//   也就是说：外部扫描会被反作弊盯上，而**在进程内读自己的内存**不会。
//
// 所以定位工作全部搬到进程内做：注入本 DLL 后，probe 会把
//   RTTI TypeDescriptor -> CompleteObjectLocator -> vtable -> 堆上活对象
// 这条链跑一遍，结果写进 C:\Xiaoyu\dump\probe.log。
#pragma once

#include <cstdint>

namespace xiaoyu {
namespace probe {

// 起一个后台线程，延迟 delay_ms 后跑一次（等游戏把对象都建出来）。
// 只跑一次，跑完线程退出。
void Start(unsigned delay_ms);

// 同步跑一遍。耗时主要在扫堆（1~2GB 全扫），几秒到十几秒。
// 会在内部把结果写 probe.log。
void Run();

// 上一次跑的结果：成功定位到的 ClientInstance 实例数（0 = 没找到）
int ClientInstanceCount();
uintptr_t FirstClientInstance();

}  // namespace probe
}  // namespace xiaoyu
