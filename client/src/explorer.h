// explorer.h - 进程内全堆实体枚举
//
// 为什么要有这个东西：
//   顺着指针链一路爬（ClientInstance -> Minecraft -> Level -> ActorManager ->
//   Dimension）已经全部走通了，但**实体表就是不在任何一层里**：
//     - Level   的直接成员里没有回指 LocalPlayer 的字段
//     - Level   里所有 vector 候选都不是实体表（最大的两个 1.5MB/2MB 是方块
//               更新队列的量级，元素大小也对不上）
//     - ActorManager 里没有 vector<Actor*>，逐 qword 当 Actor* 验只有 4 个
//               假阳性（全是 _Ref_count_resource<Pack> 里的随机浮点）
//     - Dimension 里同理，而且它的 0x2600~0x2A00 那一大片是 Molang 编译产物，
//               指针密度很低
//     - 在 ActorManager / Dimension / Level 的内存里搜 LocalPlayer 的地址，
//               零命中 —— 说明实体表不是"对象里一串 Actor*"这种形态
//   而外部扫堆会被反作弊在几秒内静默终止（已验证 4 次）。
//
// 所以换一条不依赖任何容器结构的判据：**按对象布局直接认实体**。
//
//   所有 Actor 派生类共享 Actor 基类布局，而 LocalPlayer 的字段偏移是实测出来的：
//       +0x0CE0  Vec3 mPos
//       +0x0CEC  Vec3 mPosPrev
//       +0x0DE8  BlockSource*
//   于是"这是一个 Actor"的判据变成三重硬条件：
//       1) 对象 +0 是主模块里的 vtable（堆上不会有假 vtable 恰好落在那 12MB 里）
//       2) +0xCE0 和 +0xCEC 都是合法坐标（NaN/越界/三轴全零都排除）
//       3) mPos 和 mPosPrev 的距离 < 64 米（一帧不可能跑 64 米）
//   第 2 条单独用会误报（Pack 资源里的随机浮点就骗过了），三条一起几乎不可能误报。
//
// 第二遍扫描解决"实体表在哪"这个问题 —— 既然已经拿到了若干个 Actor 的真实地址，
// 就反过来扫全堆，找哪些位置存着这些指针。同一容器里的指针会在地址上连续排列，
// 一眼就能认出来。
#pragma once

namespace xiaoyu {
namespace explorer {

// 延迟 delay_ms 后起一个后台线程跑。不阻塞调用方。
void Start(unsigned delay_ms);

// 同步跑一次完整流程（Start 内部也是调它）。
void Run();

int ActorCount();

}  // namespace explorer
}  // namespace xiaoyu
